/*
 * JTBassSynth - LV2 plugin
 *
 * A monophonic, pitch-tracking bass synthesizer effect in the spirit of the
 * MXR bass synth pedals:
 *
 *   input -> HPF -> LPF -> decimate to ~6 kHz -> YIN period detector -> glide
 *                                                                     |
 *   oscillators (saw / square / pulse / sub / octave-up / fuzz) <-----+
 *        -> 4-pole ladder filter (cutoff swept by a triggered decay env)
 *        -> VCA (follows input dynamics, shaped by Attack)
 *        -> mixed with the dry signal
 *
 * Copyright (c) 2026. ISC License.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define JTBS_HAVE_SSE 1
#endif

#include <lv2/core/lv2.h>

#define JTBS_URI "urn:jtbasssynth:bass-synth"

#define JTBS_PI 3.14159265358979323846

typedef enum {
	P_IN = 0,
	P_OUT,
	P_DRY,
	P_SYNTH,
	P_VOICE,
	P_FILTER,
	P_RESO,
	P_ATTACK,
	P_DECAY,
	P_ENVAMT,
	P_SENS,
	P_GLIDE,
	P_COUNT
} PortIndex;

typedef enum {
	V_SAW = 0,
	V_SQUARE,
	V_SAW_SUB,
	V_SQUARE_SUB,
	V_SUB,
	V_OCT_UP,
	V_PWM,
	V_FUZZ,
	V_COUNT
} Voice;

typedef struct {
	float b0, b1, b2, a1, a2;
	float z1, z2;
} Biquad;

#define YIN_BUF    1024    /* ring size in decimated samples (power of 2) */
#define YIN_RATE   6000.0  /* target analysis rate, Hz */
#define YIN_HOP    16      /* decimated samples between estimates (~2.7 ms) */
#define YIN_THRESH 0.12f   /* CMND dip threshold */
#define PITCH_DELAY 4      /* readings a small pitch change is held back (~11 ms) */
#define PITCH_SETTLE 4     /* readings after a note start before the pitch follows */
#define JUMP_N      6      /* agreeing readings needed for a jump without a pluck */

typedef struct {
	float* port[P_COUNT];
	double fs;

	/* input conditioning */
	Biquad hp_in;
	Biquad aa1, aa2;    /* anti-alias low-pass ahead of decimation */

	/* pitch tracker: YIN on a ~6 kHz decimated copy of the input */
	int      dec_factor, dec_count, hop_count;
	double   fs_d;
	int      yin_w, yin_tmin, yin_tmax;
	float    ybuf[YIN_BUF];
	uint32_t ywr;       /* decimated samples written */
	uint32_t yavail;    /* decimated samples since the last onset */
	float    yx[YIN_BUF], yd[YIN_BUF];
	bool     pitch_valid;
	bool     awaiting;   /* pluck seen, waiting for three agreeing readings */
	double   start_est[3];
	float    start_dip[3];
	float    dip_ref;    /* typical YIN dip for the sounding note */
	int      nstart;     /* consecutive clear readings since the pluck */
	double   hist[3];    /* recent accepted readings (semitones), for a median */
	int      hidx;
	double   jump_est[JUMP_N]; /* readings far from the current pitch, awaiting agreement */
	float    jump_dip[JUMP_N];
	int      njump;
	double   pq[PITCH_DELAY]; /* small pitch changes held back, dropped on a damp */
	int      pq_n, pq_head;
	int      settle;     /* readings left before the pitch follows a new note */
	double   lf, lf_tgt; /* log2(frequency) current / target */

	/* dynamics */
	float  env_fast, env_slow, env_amp;
	bool   gate;
	double since_trig;

	/* damping detection: a ripple-free envelope (peak hold, then a fast
	 * release) compared with a slower reference that follows it */
	float  env_damp, env_ref;
	float  trough;      /* lowest env_damp since the damp, to spot the next pluck */
	int    damp_hold;   /* samples left before env_damp may fall */
	float  slow_max;    /* loudest env_slow since the pluck */
	bool   attack_done; /* the note has started to decay */
	bool   decaying;    /* falling much faster than a ringing string */
	bool   damped;      /* string stopped: synth released until the next pluck */
	int    damp_age;    /* samples since the damp */
	float  ar;          /* attack/release envelope for the VCA */
	float  fenv;        /* filter sweep envelope */

	/* oscillators */
	double ph, ph_sub, ph_up, lfo;

	/* ladder filter */
	float s[4];

	/* smoothed controls */
	float dry_s, syn_s, cut_s, res_s;
} JTBassSynth;

/* ---------------------------------------------------------------- helpers */

static inline float clampf(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

static inline float coef_ms(double ms, double fs)
{
	if (ms <= 0.0) return 0.0f;
	return (float)exp(-1.0 / (ms * 0.001 * fs));
}

static void biquad_set(Biquad* b, double fs, double fc, double q, bool hp)
{
	const double w  = 2.0 * JTBS_PI * fc / fs;
	const double c  = cos(w);
	const double al = sin(w) / (2.0 * q);
	const double a0 = 1.0 + al;
	if (hp) {
		b->b0 = (float)((1.0 + c) * 0.5 / a0);
		b->b1 = (float)(-(1.0 + c) / a0);
	} else {
		b->b0 = (float)((1.0 - c) * 0.5 / a0);
		b->b1 = (float)((1.0 - c) / a0);
	}
	b->b2 = b->b0;
	b->a1 = (float)(-2.0 * c / a0);
	b->a2 = (float)((1.0 - al) / a0);
}

static inline float biquad_run(Biquad* b, float x)
{
	const float y = b->b0 * x + b->z1;
	b->z1 = b->b1 * x - b->a1 * y + b->z2;
	b->z2 = b->b2 * x - b->a2 * y;
	return y;
}

/* PolyBLEP residual for band-limited discontinuities. */
static inline double blep(double t, double dt)
{
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.0;
	}
	if (t > 1.0 - dt) {
		t = (t - 1.0) / dt;
		return t * t + t + t + 1.0;
	}
	return 0.0;
}

static inline double wrap(double p)
{
	return p - floor(p);
}

static inline double osc_saw(double ph, double dt)
{
	return 2.0 * ph - 1.0 - blep(ph, dt);
}

/* Pulse as the difference of two phase-offset saws (zero mean for any
 * width); width in (0,1), 0.5 gives a +/-1 square. */
static inline double osc_pulse(double ph, double dt, double width)
{
	return osc_saw(ph, dt) - osc_saw(wrap(ph + width), dt);
}

static inline double median3(double a, double b, double c)
{
	if (a > b) { double t = a; a = b; b = t; }
	if (b > c) { b = c; }
	return a > b ? a : b;
}

/* ------------------------------------------------------------- lifecycle */

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate,
                              const char* path, const LV2_Feature* const* f)
{
	(void)d; (void)path; (void)f;
	JTBassSynth* self = (JTBassSynth*)calloc(1, sizeof(JTBassSynth));
	if (!self) return NULL;
	self->fs = rate;
	biquad_set(&self->hp_in, rate, 28.0, 0.707, true);

	/* 4th-order Butterworth anti-alias filter, then decimate to ~6 kHz */
	biquad_set(&self->aa1, rate, 900.0, 0.5412, false);
	biquad_set(&self->aa2, rate, 900.0, 1.3066, false);
	self->dec_factor = (int)lrint(rate / YIN_RATE);
	if (self->dec_factor < 1) self->dec_factor = 1;
	self->fs_d     = rate / self->dec_factor;
	self->yin_w    = (int)lrint(0.015 * self->fs_d);     /* 15 ms window */
	self->yin_tmin = (int)floor(self->fs_d / 520.0);     /* highest note */
	self->yin_tmax = (int)ceil(self->fs_d / 28.0);       /* lowest note  */
	if (self->yin_tmin < 2) self->yin_tmin = 2;

	self->lf = self->lf_tgt = log2(55.0);
	self->cut_s = 800.0f;
	self->dry_s = 1.0f;
	self->syn_s = 0.7f;
	return (LV2_Handle)self;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data)
{
	JTBassSynth* self = (JTBassSynth*)h;
	if (port < P_COUNT) self->port[port] = (float*)data;
}

static void activate(LV2_Handle h)
{
	JTBassSynth* self = (JTBassSynth*)h;
	self->hp_in.z1 = self->hp_in.z2 = 0.0f;
	self->aa1.z1 = self->aa1.z2 = 0.0f;
	self->aa2.z1 = self->aa2.z2 = 0.0f;
	for (int i = 0; i < 4; ++i) self->s[i] = 0.0f;
	for (int i = 0; i < YIN_BUF; ++i) self->ybuf[i] = 0.0f;
	self->ywr = self->yavail = 0;
	self->dec_count = self->hop_count = 0;
	self->gate = false;
	self->ar = self->fenv = 0.0f;
	self->env_fast = self->env_slow = self->env_amp = 0.0f;
	self->env_damp = self->env_ref = self->slow_max = 0.0f;
	self->damp_hold = 0;
	self->attack_done = self->decaying = self->damped = false;
	self->pitch_valid = false;
	self->awaiting = false;
	self->nstart = 0;
}

static void cleanup(LV2_Handle h)
{
	free(h);
}

/* ------------------------------------------------------------------ DSP */

/*
 * YIN period estimate over the most recent decimated samples.
 * Only samples since the last onset are used, so the lag range grows as the
 * note rings: G-string notes resolve in ~28 ms, low E in ~44 ms. Taking the
 * first CMND dip under the threshold (not the deepest) avoids the
 * octave-low errors that the 2nd/3rd harmonics of a low string provoke.
 * Returns the period in decimated samples, or 0 if not confident yet, and
 * the dip depth in *dip (lower is cleaner).
 */
static double yin_estimate(JTBassSynth* self, float* dip)
{
	const int W = self->yin_w;
	int tmax = self->yin_tmax;
	const int avail = (int)self->yavail - W - 2;
	if (avail < tmax) tmax = avail;
	if (tmax < self->yin_tmin + 2) return 0.0;

	/* Newest sample first: x[0..W) is the most recent audio and x[j + t] is
	 * t samples earlier. With oldest-first order the reference window stayed
	 * pinned to the attack transient until the lag range filled (~50 ms),
	 * which made the first readings of a note shaky and flat. */
	const int span = W + tmax + 2;
	float* x = self->yx;
	for (int j = 0; j < span; ++j)
		x[j] = self->ybuf[(self->ywr - 1u - (uint32_t)j) & (YIN_BUF - 1)];

	/* cumulative-mean-normalised difference function, lags 1..tmax+1 */
	float* d = self->yd;
	float run = 0.0f;
	d[0] = 1.0f;
	for (int t = 1; t <= tmax + 1; ++t) {
		float acc = 0.0f;
		for (int j = 0; j < W; ++j) {
			const float e = x[j] - x[j + t];
			acc += e * e;
		}
		run += acc;
		d[t] = run > 1e-20f ? acc * (float)t / run : 1.0f;
	}

	int best = 0;
	for (int t = self->yin_tmin; t <= tmax; ++t) {
		if (d[t] < YIN_THRESH) {
			while (t <= tmax && d[t + 1] < d[t]) ++t;
			best = t;
			break;
		}
	}
	if (!best) {
		/* no clear dip: accept the global minimum only if it is decent */
		float m = 0.3f;
		for (int t = self->yin_tmin; t <= tmax; ++t)
			if (d[t] < m && d[t] <= d[t - 1] && d[t] <= d[t + 1]) { m = d[t]; best = t; }
	}
	if (!best || best > tmax) return 0.0; /* still descending: wait for data */

	const float a = d[best - 1], b = d[best], c = d[best + 1];
	const float den = a - 2.0f * b + c;
	*dip = b;
	double off = den > 1e-9f ? 0.5 * (a - c) / den : 0.0;
	if (off > 0.5) off = 0.5;
	if (off < -0.5) off = -0.5;
	return best + off;
}

static void trigger(JTBassSynth* self)
{
	self->fenv = 1.0f;
	self->since_trig = 0.0;
	/* Analyse only the new note; keep sounding the old pitch until then. */
	self->yavail = 0;
	self->hop_count = 0;
	self->awaiting = true;
	self->nstart = 0;
	self->njump = 0;
	self->slow_max = self->env_slow;
	self->attack_done = false;
	self->decaying = false;
	self->damped = false;
}

/*
 * Handle one pitch reading (fractional MIDI semitones).
 *
 * After a pluck, the oscillator only moves once three consecutive clear
 * readings agree: within 0.12 semitone (rock steady, even if the bass is out
 * of tune), or within 0.3 and close to a semitone. Until then it stays
 * silent (from silence) or keeps the previous pitch (re-pluck), so a shaky
 * first reading is never heard.
 *
 * The note starts on the nearest semitone (the first readings run a little
 * flat), unless the readings are rock steady and clearly between semitones
 * (an out-of-tune bass), in which case it starts where they are.
 *
 * Once the note is sounding, the oscillator follows the median of the last
 * three readings, skipping low-confidence ones (dip > 0.2) and any taken
 * while the string is being damped, where fret noise and the open string
 * sneak in as a finger lifts off. Small changes are held back by
 * PITCH_DELAY readings and dropped if a damp is spotted in that time, since
 * the readings drift sharp in the last ~10 ms before a damp.
 */
static void on_estimate(JTBassSynth* self, double semi, float dip)
{
	if (self->awaiting) {
		if (dip > YIN_THRESH) {
			self->nstart = 0;
			return;
		}
		self->start_est[self->nstart % 3] = semi;
		self->start_dip[self->nstart % 3] = dip;
		self->nstart++;
		if (self->nstart < 3) return;
		const double a = self->start_est[0], b = self->start_est[1], c = self->start_est[2];
		const double lo = fmin(a, fmin(b, c)), hi = fmax(a, fmax(b, c));
		const double avg = (a + b + c) / 3.0;
		const double off = fabs(avg - (double)lrint(avg));
		if (!(hi - lo < 0.12 || (hi - lo < 0.3 && off < 0.25))) return;
		const double start = off < 0.25 ? (double)lrint(avg) : avg;
		self->dip_ref = fmaxf(0.002f, fmaxf(self->start_dip[0], fmaxf(self->start_dip[1], self->start_dip[2])));
		self->awaiting = false;
		self->hist[0] = self->hist[1] = self->hist[2] = start;
		self->pq_n = 0;
		self->settle = PITCH_SETTLE;
		self->lf_tgt = log2(440.0) + (start - 69.0) / 12.0;
		if (!self->pitch_valid) self->lf = self->lf_tgt; /* no glide from silence */
		self->pitch_valid = true;
		return;
	}
	if (!self->pitch_valid) return;
	if (self->decaying) {
		self->pq_n = 0; /* the string is being damped: drop held-back changes */
		return;
	}
	/* A jump of more than 0.75 semitone (hammer-on, pull-off, a pluck too
	 * soft to be detected as one, or a harmonic misread twice in a row,
	 * which a median of three can't reject) only moves the oscillator once
	 * JUMP_N clear readings in a row (~16 ms) agree on it. Readings taken
	 * while the next finger touches the string before a damp keep drifting
	 * and never pass. */
	const double cur = 69.0 + 12.0 * (self->lf_tgt - log2(440.0));
	if (fabs(semi - cur) > 0.75) {
		if (dip >= YIN_THRESH) {
			self->njump = 0;
			return;
		}
		self->jump_est[self->njump % JUMP_N] = semi;
		self->jump_dip[self->njump % JUMP_N] = dip;
		if (++self->njump < JUMP_N) return;
		double lo = 1e9, hi = -1e9, sum = 0.0;
		float  dmax = 0.002f;
		for (int k = 0; k < JUMP_N; ++k) {
			lo = fmin(lo, self->jump_est[k]);
			hi = fmax(hi, self->jump_est[k]);
			sum += self->jump_est[k];
			dmax = fmaxf(dmax, self->jump_dip[k]);
		}
		if (hi - lo >= 0.3) return;
		/* confirmed jump: move now, and let the new pitch settle */
		const double j = sum / JUMP_N;
		self->hist[0] = self->hist[1] = self->hist[2] = j;
		self->njump = 0;
		self->pq_n = 0;
		self->settle = PITCH_SETTLE;
		self->dip_ref = dmax;
		self->lf_tgt = log2(440.0) + (j - 69.0) / 12.0;
		return;
	}
	self->njump = 0;

	/* Small changes: each note learns its typical dip (cleanness), and
	 * readings much less clean than that are ignored. When the next finger
	 * touches the string, ~30 ms before the pluck and before the level
	 * drops, the dip jumps 10x and the pitch slides 0.3-0.8 semitone. A
	 * noisy pickup just has a higher typical dip. */
	if (dip > 0.2f || dip > 3.0f * self->dip_ref + 0.004f) return;
	self->dip_ref += 0.2f * (dip - self->dip_ref);

	self->hist[self->hidx] = semi;
	self->hidx = (self->hidx + 1) % 3;
	const double m = median3(self->hist[0], self->hist[1], self->hist[2]);
	if (self->settle > 0) {
		self->settle--;
		return;
	}
	if (self->pq_n == PITCH_DELAY) {
		self->lf_tgt = log2(440.0) + (self->pq[self->pq_head] - 69.0) / 12.0;
		self->pq_head = (self->pq_head + 1) % PITCH_DELAY;
		self->pq_n--;
	}
	self->pq[(self->pq_head + self->pq_n) % PITCH_DELAY] = m;
	self->pq_n++;
}

static void run(LV2_Handle h, uint32_t n)
{
	JTBassSynth* self = (JTBassSynth*)h;
	const double fs = self->fs;

#ifdef JTBS_HAVE_SSE
	const unsigned int csr = _mm_getcsr();
	_mm_setcsr(csr | 0x8040); /* FTZ | DAZ */
#endif

	const float* in  = self->port[P_IN];
	float*       out = self->port[P_OUT];

	const float dry    = clampf(*self->port[P_DRY], 0.0f, 1.0f);
	const float synth  = clampf(*self->port[P_SYNTH], 0.0f, 1.0f);
	int voice          = (int)lrintf(*self->port[P_VOICE]);
	const float cutoff = clampf(*self->port[P_FILTER], 40.0f, 10000.0f);
	const float reso   = clampf(*self->port[P_RESO], 0.0f, 1.0f);
	const float att_ms = clampf(*self->port[P_ATTACK], 1.0f, 2000.0f);
	const float dec_ms = clampf(*self->port[P_DECAY], 10.0f, 5000.0f);
	const float envamt = clampf(*self->port[P_ENVAMT], 0.0f, 1.0f);
	const float sens   = clampf(*self->port[P_SENS], -80.0f, -10.0f);
	const float glide  = clampf(*self->port[P_GLIDE], 0.0f, 1000.0f);
	if (voice < 0 || voice >= V_COUNT) voice = V_SAW;

	/* envelope / smoothing coefficients */
	const float fast_a = coef_ms(0.3, fs),  fast_r = coef_ms(12.0, fs);
	const float slow_a = coef_ms(25.0, fs), slow_r = coef_ms(250.0, fs);
	const float amp_a  = coef_ms(2.0, fs),  amp_r  = coef_ms(70.0, fs);
	const float ar_a   = 1.0f - coef_ms(att_ms / 3.0, fs); /* ~95% at att_ms */
	const float ar_r   = 1.0f - coef_ms(35.0, fs);
	const float fdec   = coef_ms(dec_ms / 3.0, fs);
	const float gl     = 1.0f - coef_ms(glide > 0.5f ? glide / 2.0 : 0.25, fs);
	const float smooth = 1.0f - coef_ms(20.0, fs);
	const float thr_on  = powf(10.0f, sens / 20.0f);
	const float thr_off = thr_on * 0.5f;
	const double holdoff = 0.07 * fs;
	const double lfo_inc = 0.6 / fs;
	const int   damp_hold_n = (int)(0.012 * fs);
	const float damp_r = coef_ms(12.0, fs);
	const float ref_a  = coef_ms(10.0, fs), ref_r = coef_ms(80.0, fs);
	const double damp_min_age = 0.1 * fs;
	const int   damp_grace_n = (int)(0.04 * fs);

	for (uint32_t i = 0; i < n; ++i) {
		const float x  = in[i];
		const float xh = biquad_run(&self->hp_in, x);
		const float ax = fabsf(xh);

		/* --- envelope followers ------------------------------------- */
		self->env_fast = ax + (self->env_fast - ax) * (ax > self->env_fast ? fast_a : fast_r);
		self->env_slow = ax + (self->env_slow - ax) * (ax > self->env_slow ? slow_a : slow_r);
		self->env_amp  = ax + (self->env_amp  - ax) * (ax > self->env_amp  ? amp_a  : amp_r);

		/* Damping envelope: hold each peak for 12 ms (longer than half a
		 * cycle of low B, so it doesn't ripple), then fall with 12 ms. */
		if (ax >= self->env_damp) {
			self->env_damp  = ax;
			self->damp_hold = damp_hold_n;
		} else if (self->damp_hold > 0) {
			self->damp_hold--;
		} else {
			self->env_damp *= damp_r;
		}
		self->env_ref = self->env_damp + (self->env_ref - self->env_damp) *
		                (self->env_damp > self->env_ref ? ref_a : ref_r);

		/* Attack phase ends once the slow envelope has dropped 1 dB below
		 * its peak. A light, slowly building pluck stays in its attack. */
		if (self->env_slow > self->slow_max) self->slow_max = self->env_slow;
		else if (self->env_slow < 0.89f * self->slow_max) self->attack_done = true;

		/* A ringing string decays at a few tens of dB per second, which
		 * keeps env_damp within ~3-7 dB of env_ref. Lifting a finger off a
		 * fret drops it far faster. Below -6 dB, pitch readings are ignored.
		 * (They only steer the pitch, so a fast natural decay that trips this
		 * just holds the pitch it already has.) */
		if (self->damped && self->env_damp < self->trough) self->trough = self->env_damp;
		self->decaying = self->gate && self->attack_done &&
		                 self->env_damp < 0.5f * self->env_ref;

		/* --- gate & note-onset detection ----------------------------- */
		self->since_trig += 1.0;
		if (!self->gate && self->env_amp > thr_on) {
			self->gate = true;
			self->pitch_valid = false;
			trigger(self);
		} else if (self->gate && self->env_amp < thr_off) {
			self->gate = false;
			self->awaiting = false;
		} else if (self->gate && self->damped &&
		           self->env_damp > 4.0f * self->trough && self->env_fast > 2.0f * thr_on) {
			/* Next pluck after a damp: a 12 dB rise from the bottom of the
			 * dip. Fingerstyle players damp each note with the next finger
			 * for 15-25 ms just before plucking, too briefly for env_slow
			 * to fall, so the rule below would miss it. */
			trigger(self);
		} else if (self->gate && self->attack_done && self->since_trig > holdoff &&
		           self->env_fast > 2.0f * self->env_slow &&
		           self->env_fast > 2.0f * thr_on) {
			/* New pluck while still ringing. Requiring the note to have
			 * started decaying stops a soft attack that is still building
			 * from firing the sweep twice. */
			trigger(self);
		} else if (self->gate && !self->damped && self->attack_done &&
		           self->since_trig > damp_min_age &&
		           self->env_damp < 0.2f * self->env_ref) {
			/* String damped (finger lifted, or the next finger landing):
			 * freeze the pitch, and release the synth after a 40 ms grace
			 * period unless another pluck comes first. Then the old pitch
			 * runs straight into the new one, as on a re-pluck. */
			self->damped = true;
			self->trough = self->env_damp;
			self->awaiting = false;
			self->damp_age = 0;
			self->pq_n = 0;
		}
		if (self->damped && self->damp_age <= damp_grace_n) self->damp_age++;

		/* --- pitch tracker ------------------------------------------ */
		const float xa = biquad_run(&self->aa2, biquad_run(&self->aa1, xh));
		if (++self->dec_count >= self->dec_factor) {
			self->dec_count = 0;
			self->ybuf[self->ywr & (YIN_BUF - 1)] = xa;
			self->ywr++;
			if (self->gate) {
				if (self->yavail < YIN_BUF - 1) self->yavail++;
				if (++self->hop_count >= YIN_HOP) {
					self->hop_count = 0;
					float dip = 1.0f;
					const double p = yin_estimate(self, &dip);
					if (p > 0.0 && !self->damped)
						on_estimate(self, 69.0 + 12.0 * log2(self->fs_d / p / 440.0), dip);
				}
			}
		}

		self->lf += (self->lf_tgt - self->lf) * gl;
		const double f  = exp2(self->lf);

		/* --- oscillators -------------------------------------------- */
		const double dt = f / fs;
		self->ph     = wrap(self->ph + dt);
		self->ph_sub = wrap(self->ph_sub + dt * 0.5);
		self->ph_up  = wrap(self->ph_up + dt * 2.0);
		self->lfo    = wrap(self->lfo + lfo_inc);

		double sig;
		switch (voice) {
		default:
		case V_SAW:
			sig = osc_saw(self->ph, dt);
			break;
		case V_SQUARE:
			sig = osc_pulse(self->ph, dt, 0.5);
			break;
		case V_SAW_SUB:
			sig = 0.6 * osc_saw(self->ph, dt) + 0.7 * osc_pulse(self->ph_sub, dt * 0.5, 0.5);
			break;
		case V_SQUARE_SUB:
			sig = 0.55 * osc_pulse(self->ph, dt, 0.5) + 0.7 * osc_pulse(self->ph_sub, dt * 0.5, 0.5);
			break;
		case V_SUB:
			sig = osc_pulse(self->ph_sub, dt * 0.5, 0.5);
			break;
		case V_OCT_UP:
			sig = 0.7 * osc_saw(self->ph_up, dt * 2.0) + 0.45 * osc_saw(self->ph, dt);
			break;
		case V_PWM: {
			const double w = 0.5 + 0.38 * sin(2.0 * JTBS_PI * self->lfo);
			sig = osc_pulse(self->ph, dt, w);
			break;
		}
		case V_FUZZ:
			/* Squared-up input, analog octave-fuzz style: no tracking lag. */
			sig = tanh(xh * 60.0);
			break;
		}

		/* --- filter ------------------------------------------------- */
		self->cut_s += (cutoff - self->cut_s) * smooth;
		self->res_s += (reso - self->res_s) * smooth;

		const double keytrk = pow(f / 110.0, 0.3);
		double fc = self->cut_s * keytrk * exp2(envamt * 5.5 * self->fenv);
		if (fc > fs * 0.45) fc = fs * 0.45;
		if (fc < 20.0) fc = 20.0;

		const float g  = (float)tan(JTBS_PI * fc / fs);
		const float G  = g / (1.0f + g);
		const float b  = 1.0f - G;
		const float k  = self->res_s * 3.95f;
		const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
		const float S  = G3 * b * self->s[0] + G2 * b * self->s[1] + G * b * self->s[2] + b * self->s[3];
		const float u  = tanhf((float)sig * (1.0f + 0.5f * k) * 0.9f);
		const float y4 = (G4 * u + S) / (1.0f + k * G4);
		float v = u - k * y4;
		for (int st = 0; st < 4; ++st) {
			const float vv = (v - self->s[st]) * G;
			const float y  = vv + self->s[st];
			self->s[st] = y + vv;
			v = y;
		}
		const float filt = v;

		self->fenv *= fdec;

		/* --- VCA ---------------------------------------------------- */
		const bool released = self->damped && self->damp_age > damp_grace_n;
		const bool sounding = self->gate && !released && (self->pitch_valid || voice == V_FUZZ);
		if (sounding)
			self->ar += (1.0f - self->ar) * ar_a;
		else
			self->ar -= self->ar * ar_r;

		const float dyn = self->env_amp / (self->env_amp + 0.04f) * 1.04f;
		const float syn = tanhf(filt * 1.4f) * self->ar * dyn;

		/* --- mix ---------------------------------------------------- */
		self->dry_s += (dry - self->dry_s) * smooth;
		self->syn_s += (synth - self->syn_s) * smooth;
		out[i] = self->dry_s * x + self->syn_s * syn * 0.8f;
	}

#ifdef JTBS_HAVE_SSE
	_mm_setcsr(csr);
#endif
}

static const void* extension_data(const char* uri)
{
	(void)uri;
	return NULL;
}

static const LV2_Descriptor descriptor = {
	JTBS_URI,
	instantiate,
	connect_port,
	activate,
	run,
	NULL, /* deactivate */
	cleanup,
	extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
	return index == 0 ? &descriptor : NULL;
}
