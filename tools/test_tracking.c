/*
 * Offline test for JTBassSynth: synthetic bass plucks in, synth state out.
 *
 * Builds the plugin source directly (no host needed), runs it in 128-frame
 * blocks at 48 kHz, and after each block reads the oscillator pitch, the
 * VCA level and the filter-sweep envelope. Per note it checks that the synth
 * never sounds a wrong pitch (not even briefly at the start), that the
 * filter sweep fires once per pluck, and how long the synth takes to sound.
 * It also covers light plucks, finger lifts, re-plucks and hammer-ons.
 *
 *   make test
 */

#include "../JTBassSynth.c"

#include <stdio.h>
#include <string.h>

#define FS     48000
#define BLOCK  128
#define MAXLEN (FS * 8)
#define NBLK   (MAXLEN / BLOCK + 1)

/* ------------------------------------------------------------- harness */

/* Per-block state after each run() call. */
static double pitch[NBLK];   /* oscillator pitch, MIDI semitones */
static float  vca[NBLK];     /* VCA (attack/release) level */
static bool   swept[NBLK];   /* filter sweep (re)triggered in this block */
static int    nblk;

typedef struct {
	float dry, synth, voice, filter, reso, attack, decay, env, sens, glide;
} Controls;

static Controls defaults(void)
{
	Controls c = { 1.0f, 0.7f, 2.0f, 300.0f, 0.45f, 5.0f, 350.0f, 0.6f, -48.0f, 0.0f };
	return c;
}

static void run_plugin(const float* in, long len, Controls c)
{
	JTBassSynth* self = (JTBassSynth*)descriptor.instantiate(&descriptor, FS, "", NULL);
	static float out[BLOCK];
	float* ctl = &c.dry;
	descriptor.connect_port(self, P_OUT, out);
	for (int i = 0; i < 10; ++i) descriptor.connect_port(self, P_DRY + i, ctl + i);
	descriptor.activate(self);

	nblk = 0;
	for (long pos = 0; pos < len; pos += BLOCK) {
		const uint32_t n = (uint32_t)(len - pos < BLOCK ? len - pos : BLOCK);
		const float fenv_before = self->fenv;
		descriptor.connect_port(self, P_IN, (void*)(in + pos));
		descriptor.run(self, n);
		pitch[nblk] = 69.0 + 12.0 * (self->lf - log2(440.0));
		vca[nblk]   = self->ar;
		swept[nblk] = self->fenv > fenv_before + 1e-4f;
		nblk++;
	}
	descriptor.cleanup(self);
}

static long blk(long frame) { return frame / BLOCK; }
static double ms(long blocks) { return blocks * BLOCK * 1000.0 / FS; }

/* --------------------------------------------------------- pluck model */

typedef struct { const char* name; float h[8]; } Mix;

static const Mix MIXES[] = {
	{ "round",     { 1.00f, 0.50f, 0.30f, 0.15f, 0.08f, 0.05f, 0.03f, 0.02f } },
	{ "bright",    { 1.00f, 0.80f, 0.70f, 0.50f, 0.40f, 0.30f, 0.20f, 0.15f } },
	{ "weak fund", { 0.35f, 1.00f, 0.80f, 0.40f, 0.25f, 0.15f, 0.08f, 0.05f } },
};
#define NMIX (int)(sizeof(MIXES) / sizeof(MIXES[0]))

static uint32_t rng = 1;
static float noise(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (float)((rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

static double midi_hz(double note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }

/* Plucked note: stiff-string partials, pick click, 15 ms damp at the end;
 * optional pitch change mid-note without a new attack (hammer-on). */
static void pluck(float* buf, long start, long dur, double note, float amp,
                  const Mix* mix, double glide_to, long glide_at)
{
	double ph[8] = { 0 };
	const double B = 0.00012;
	const long damp = FS * 15 / 1000;
	for (long i = 0; i < dur; ++i) {
		const double t = (double)i / FS;
		const double f = midi_hz(glide_at > 0 && i >= glide_at ? glide_to : note);
		double s = 0.0;
		for (int k = 0; k < 8; ++k) {
			const double fk = (k + 1) * f * sqrt(1.0 + B * (k + 1) * (k + 1));
			if (fk > 0.45 * FS) continue;
			ph[k] += fk / FS;
			ph[k] -= floor(ph[k]);
			s += mix->h[k] * exp(-t * (1.2 + 0.9 * k)) * sin(2.0 * JTBS_PI * ph[k]);
		}
		float a = amp * (float)(1.0 - exp(-t / 0.0008));
		if (i > dur - damp) a *= (float)(dur - i) / damp;
		float click = i < FS / 300 ? 0.08f * amp * noise() : 0.0f;
		buf[start + i] += a * (float)s * 0.45f + click;
	}
}

/* Light pluck swelling in over attack_ms, then a finger lifted off the fret
 * after dur samples: the note dies (15 ms), a fret click sounds, and the
 * open string rings on quietly underneath (-26 dB, 150 ms decay). */
static void pluck_lift(float* buf, long start, long dur, double note, double open_note,
                       float amp, double attack_ms)
{
	double ph[8] = { 0 }, po[4] = { 0 };
	const double B = 0.00012;
	const long att = (long)(attack_ms * 0.001 * FS);
	const long tail = FS / 3;
	for (long i = 0; i < dur + tail; ++i) {
		const double t = (double)i / FS;
		const double f = midi_hz(note);
		double s = 0.0;
		for (int k = 0; k < 8; ++k) {
			const double fk = (k + 1) * f * sqrt(1.0 + B * (k + 1) * (k + 1));
			ph[k] += fk / FS;
			ph[k] -= floor(ph[k]);
			s += MIXES[0].h[k] * exp(-t * (1.2 + 0.9 * k)) * sin(2.0 * JTBS_PI * ph[k]);
		}
		double a = i < att ? 0.5 - 0.5 * cos(JTBS_PI * i / att) : 1.0;
		double y = 0.0;
		if (i >= dur) {
			const double tl = (double)(i - dur) / FS;
			a *= exp(-tl / 0.015);
			const double fo = midi_hz(open_note);
			double so = 0.0;
			for (int k = 0; k < 4; ++k) {
				po[k] += (k + 1) * fo / FS;
				po[k] -= floor(po[k]);
				so += MIXES[0].h[k] * sin(2.0 * JTBS_PI * po[k]);
			}
			y += 0.05 * amp * (1.0 - exp(-tl / 0.003)) * exp(-tl / 0.15) * so * 0.45;
			if (i - dur < FS / 200) y += 0.15 * amp * noise() * exp(-tl / 0.0015);
		}
		buf[start + i] += (float)(amp * a * s * 0.45 + y);
	}
}

/* --------------------------------------------------------------- checks */

static int failures;

static void check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok) failures++;
}

/* Audible = VCA above -20 dB. */
#define AUDIBLE 0.1f

static float buf[MAXLEN];

/* Over blocks [b0, b1): first audible block (or -1), the largest pitch error
 * while audible, and the number of filter sweeps. */
static void scan(long b0, long b1, double note, long* first, double* maxdev, int* sweeps)
{
	*first = -1; *maxdev = 0.0; *sweeps = 0;
	for (long b = b0; b < b1 && b < nblk; ++b) {
		if (swept[b]) (*sweeps)++;
		if (vca[b] > AUDIBLE) {
			if (*first < 0) *first = b;
			const double dev = fabs(pitch[b] - note);
			if (dev > *maxdev) *maxdev = dev;
		}
	}
}

static void test_strings(void)
{
	static const struct { const char* name; int open; int frets; } strings[] = {
		{ "B (5-str)", 23, 12 }, { "E", 28, 12 }, { "A", 33, 12 },
		{ "D", 38, 12 }, { "G", 43, 24 },
	};
	static const int frets[] = { 0, 1, 3, 5, 7, 9, 12, 15, 17, 19, 21, 24 };

	printf("\nSingle notes: time until the synth sounds (ms), pitch while sounding\n");
	printf("  %-10s %-10s %6s %6s %6s %s\n", "string", "tone", "min", "avg", "max", "wrong pitch / extra sweeps");

	for (size_t s = 0; s < sizeof(strings) / sizeof(strings[0]); ++s) {
		for (int m = 0; m < NMIX; ++m) {
			double lsum = 0.0, lmin = 1e9, lmax = 0.0;
			int count = 0, bad = 0;
			for (size_t fi = 0; fi < sizeof(frets) / sizeof(frets[0]); ++fi) {
				if (frets[fi] > strings[s].frets) continue;
				const int note = strings[s].open + frets[fi];
				const long onset = FS / 5, len = FS;
				memset(buf, 0, sizeof(buf));
				pluck(buf, onset, FS / 2, note, 0.3f, &MIXES[m], 0, 0);
				run_plugin(buf, len, defaults());

				long first; double maxdev; int sweeps;
				/* stop checking pitch 20 ms before the end damp */
				scan(blk(onset), blk(onset + FS / 2 - FS / 50), note, &first, &maxdev, &sweeps);
				if (first >= 0) {
					const double l = ms(first) - onset * 1000.0 / FS;
					lsum += l; count++;
					if (l < lmin) lmin = l;
					if (l > lmax) lmax = l;
				}
				if (first < 0 || maxdev > 0.5 || sweeps != 1) {
					bad++;
					printf("    ! %s fret %d (note %d, %s): %s, pitch off by up to %.2f semitones, %d sweeps\n",
					       strings[s].name, frets[fi], note, MIXES[m].name,
					       first < 0 ? "never sounded" : "sounded", maxdev, sweeps);
				}
			}
			printf("  %-10s %-10s %6.1f %6.1f %6.1f %d\n", strings[s].name, MIXES[m].name,
			       count ? lmin : 0.0, count ? lsum / count : 0.0, count ? lmax : 0.0, bad);
			if (bad) failures++;
		}
	}
}

static void test_light_pluck_and_lift(void)
{
	static const struct { int note, open; const char* name; } cases[] = {
		{ 31, 28, "G1 (E string, fret 3)" }, { 36, 33, "C2 (A string, fret 3)" },
		{ 40, 38, "E2 (D string, fret 2)" }, { 47, 43, "B2 (G string, fret 4)" },
	};
	printf("\nLight pluck (40 ms swell, -26 dB) then finger lifted at 600 ms\n");
	for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
		const long onset = FS / 5, lift = onset + FS * 3 / 5, len = FS * 3 / 2;
		memset(buf, 0, sizeof(buf));
		pluck_lift(buf, onset, lift - onset, cases[k].note, cases[k].open, 0.05f, 40.0);
		run_plugin(buf, len, defaults());

		long first; double maxdev; int sweeps;
		scan(blk(onset), nblk, cases[k].note, &first, &maxdev, &sweeps);
		/* The VCA releases with its own 35 ms time constant, so check when
		 * the release starts (VCA 1 dB down), not when it is silent. It waits
		 * out a 40 ms grace period after the damp, in case of a re-pluck. */
		long rel = -1;
		const float at_lift = vca[blk(lift)];
		for (long b = blk(lift); b < nblk; ++b)
			if (vca[b] < 0.89f * at_lift) { rel = b; break; }
		const double rel_ms = rel < 0 ? -1.0 : ms(rel) - lift * 1000.0 / FS;
		char what[200];
		snprintf(what, sizeof what,
		         "%s: %d sweep, pitch off by up to %.2f semitones, release starts %.0f ms after lift",
		         cases[k].name, sweeps, maxdev, rel_ms);
		check(sweeps == 1 && first >= 0 && maxdev < 0.5 && rel_ms >= 0.0 && rel_ms < 90.0, what);
	}
}

static void test_legato(void)
{
	printf("\nRe-pluck while the previous note rings (E1 -> A1 -> D2)\n");
	const long len = FS * 2;
	const long t1 = FS / 5, t2 = FS * 3 / 5, t3 = FS;
	memset(buf, 0, sizeof(buf));
	pluck(buf, t1, t2 - t1 + FS / 50, 28, 0.3f, &MIXES[0], 0, 0);
	pluck(buf, t2, t3 - t2 + FS / 50, 33, 0.3f, &MIXES[0], 0, 0);
	pluck(buf, t3, FS / 2, 38, 0.3f, &MIXES[0], 0, 0);
	run_plugin(buf, len, defaults());
	int sweeps = 0;
	for (long b = 0; b < nblk; ++b) if (swept[b]) sweeps++;
	/* pitch 100 ms into each note */
	const double p1 = pitch[blk(t1 + FS / 10)], p2 = pitch[blk(t2 + FS / 10)], p3 = pitch[blk(t3 + FS / 10)];
	printf("  %d sweeps; pitch 100 ms in: %.2f, %.2f, %.2f\n", sweeps, p1, p2, p3);
	check(sweeps == 3, "one filter sweep per pluck");
	check(fabs(p1 - 28) < 0.3 && fabs(p2 - 33) < 0.3 && fabs(p3 - 38) < 0.3, "each note at the right pitch");
}

static void test_hammer_on(void)
{
	printf("\nHammer-on without a new attack (A1 -> C2 at 300 ms)\n");
	const long onset = FS / 5, ham = FS * 3 / 10, len = FS + FS / 2;
	memset(buf, 0, sizeof(buf));
	pluck(buf, onset, FS, 33, 0.3f, &MIXES[0], 36, ham);
	run_plugin(buf, len, defaults());
	int sweeps = 0;
	for (long b = 0; b < nblk; ++b) if (swept[b]) sweeps++;
	long settled = -1;
	for (long b = blk(onset + ham); b < nblk; ++b)
		if (fabs(pitch[b] - 36) < 0.3) { settled = b; break; }
	const double p_before = pitch[blk(onset + ham) - 2];
	if (settled >= 0)
		printf("  pitch reaches C2 %.1f ms after the hammer-on\n", ms(settled) - (onset + ham) * 1000.0 / FS);
	check(sweeps == 1, "no filter sweep from the hammer-on");
	check(fabs(p_before - 33) < 0.3 && settled >= 0, "A1, then C2");
}

/* The scale from a real recording that exposed a bug: G major up and down,
 * fingerstyle, each note damped by the next finger for ~20 ms just before
 * it is plucked. Expect one filter sweep per note, the right pitch on each
 * note, and no dropout between notes. */
static void test_fingerstyle_scale(void)
{
	static const int scale[] = { 31, 33, 35, 36, 38, 40, 42, 43, 42, 40, 38, 36, 35, 33, 31 };
	const int ns = (int)(sizeof(scale) / sizeof(scale[0]));
	const long step = FS * 38 / 100, dur = FS * 36 / 100, t0 = FS / 5;
	const long len = t0 + step * ns + FS / 2;
	printf("\nFingerstyle scale, each note damped ~20 ms before the next pluck\n");
	memset(buf, 0, sizeof(buf));
	for (int k = 0; k < ns; ++k)
		pluck(buf, t0 + k * step, dur, scale[k], 0.3f, &MIXES[0], 0, 0);
	run_plugin(buf, len, defaults());

	int sweeps = 0, wrong = 0;
	for (long b = 0; b < nblk; ++b) if (swept[b]) sweeps++;
	for (int k = 0; k < ns; ++k) {
		/* pitch from 80 ms into each note until its damp starts */
		for (long b = blk(t0 + k * step + FS * 8 / 100); b < blk(t0 + k * step + dur - FS / 50); ++b)
			if (vca[b] > AUDIBLE && fabs(pitch[b] - scale[k]) > 0.5) { wrong++; break; }
	}
	float vmin = 1.0f; /* lowest VCA level between the first and last note */
	for (long b = blk(t0 + FS / 10); b < blk(t0 + (ns - 1) * step + FS / 10); ++b)
		if (vca[b] < vmin) vmin = vca[b];
	printf("  %d sweeps, %d notes with a wrong pitch, VCA never below %.2f between notes\n", sweeps, wrong, vmin);
	check(sweeps == ns, "one filter sweep per note");
	check(wrong == 0, "every note at the right pitch");
	check(vmin > 0.5f, "no dropout between notes");
}

int main(void)
{
	printf("JTBassSynth offline tracking test (%d Hz, %d-frame blocks)\n", FS, BLOCK);
	printf("Latency excludes audio interface and buffer latency (~3-6 ms on a pi-Stomp).\n");
	test_strings();
	test_light_pluck_and_lift();
	test_fingerstyle_scale();
	test_legato();
	test_hammer_on();
	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
