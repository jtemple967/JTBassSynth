# JTBassSynth (LV2)

A monophonic, pitch-tracking bass synth effect modeled on MXR-style bass synth pedals.
It takes mono audio in and sends mono audio out. It has eight voices, a resonant 4-pole
ladder filter with an envelope sweep, an attack swell, glide and separate Dry and Synth levels.

## Signal flow

```
bass in ─┬──────────────────────────────────────────────── Dry ──┐
         │                                                       ├─> out
         └─ HPF ─ LPF ─ ↓6 kHz ─ YIN pitch detector ─ glide      │
                                                │                │
           voice oscillators (PolyBLEP) <───────┘                │
             └─ ladder filter (cutoff + Sweep·decay env) ─ VCA ─ Synth
                                                         ▲
                       input envelope × Attack swell ────┘
```

* **Tracking:** a YIN period detector runs on a copy of the input downsampled to about 6 kHz.
  It only analyses audio since the latest pluck, using the newest 15 ms as its reference, so
  the attack transient drops out quickly. Strong 2nd and 3rd harmonics, typical of low
  strings, don't cause octave errors.
* **Note start:** the synth sounds once three consecutive clear pitch readings agree (within
  0.12 semitone, or within 0.3 and close to a semitone). Until then it stays silent, or keeps
  the previous pitch on a re-pluck, so a shaky first reading is never heard.
* **Following the pitch:** each note starts on the nearest semitone (the first readings run
  slightly flat), and the oscillator starts following about 10 ms later. It follows the
  median of the last three readings. Each note learns how clean its readings normally are,
  and much less clean readings are ignored; those come about 30 ms before the next pluck,
  when the next finger touches the string. Small changes are held back by about 11 ms and
  dropped if a damp is detected in that time, so the sharp drift in the last moments of a
  note is never heard. A jump of more than 0.75 semitone (hammer-on, pull-off, a soft
  pluck) needs six clear readings in a row (about 16 ms) that agree, so a harmonic misread
  or the finger-touch drift can't make the synth blip.
* **Retrigger:** the filter sweep fires when the gate opens. It also fires on a new pluck
  while the string is still ringing, detected as a jump in the fast envelope relative to the
  slow envelope. That only counts once the previous note has started to decay, so a light
  pluck that swells in slowly fires the sweep once, not twice. Right after a damp, a 12 dB
  rise from the bottom of the dip counts as the next pluck, which catches fingerstyle
  playing where the next finger damps the string for 15–25 ms just before plucking.
* **Damping:** when the string is stopped, for example by lifting a finger off the fret, the
  level falls far faster than a ringing string decays. The synth freezes its pitch at once
  and releases 40 ms later unless another pluck comes first, in which case it runs straight
  into the next note. Fret noise and the open string are never followed.
* **VCA:** follows your playing dynamics, so the synth breathes like the pedal does.

## Controls

| Control        | Range          | Notes |
|----------------|----------------|-------|
| Dry            | 0–1            | Clean bass level |
| Synth          | 0–1            | Synth level |
| Voice          | 8 positions    | Saw · Square · Saw+Sub · Square+Sub · Sub Octave · Octave Up · PWM · Fuzz Square |
| Filter         | 40–8000 Hz     | Base cutoff (with light keyboard tracking) |
| Resonance      | 0–1            | Close to self-oscillation at max |
| Attack         | 1–1500 ms      | Swell-in time on each note |
| Decay          | 20–4000 ms     | Filter sweep decay time |
| Sweep          | 0–1            | Envelope amount, up to about 5.5 octaves |
| Gate Threshold | −80 to −10 dB  | Lower values make the gate more sensitive |
| Glide          | 0–500 ms       | Portamento between tracked notes |

*Fuzz Square* squares up the input directly instead of tracking it, so it has zero latency and
handles chords and double stops the way an analog octave fuzz does.

## Build & install

You need a C compiler and the LV2 headers (`lv2-dev` on Debian/Ubuntu, `lv2` on Arch/Homebrew).

```bash
make
make install        # → ~/.lv2 (Linux), ~/Library/Audio/Plug-Ins/LV2 (macOS), %APPDATA%/LV2 (MinGW)
```

URI: `urn:jtbasssynth:bass-synth`. The plugin works in Ardour, Carla, Reaper (with LV2 enabled),
Qtractor, MOD Dev, guitarix/jalv and similar hosts.

## Offline test

`make test` builds `tools/test_tracking.c`, which includes the plugin source directly (no
host needed), feeds it synthetic plucks and reads the synth's pitch, VCA and filter-sweep
state after every block. It checks:

* every fret position on the B, E, A, D and G strings in three tones (round, bright, and a
  weak-fundamental tone that is the classic octave trap): the synth never sounds a wrong
  pitch, not even briefly at the start; one filter sweep per pluck; time until it sounds
* a light pluck that swells in over 40 ms, then a finger lifted off the fret with a fret click
  and the open string ringing underneath: one sweep, steady pitch, release starts within
  90 ms of the lift (40 ms of that is the grace period for a re-pluck)
* a fingerstyle G major scale up and down, each note damped ~20 ms before the next pluck
  (modelled on a real recording): one sweep and the right pitch per note, no dropout
* re-plucks while the previous note rings: one sweep each, right pitch each
* a hammer-on with no new attack: no sweep, pitch moves to the new note

It exits non-zero if anything fails.

## MOD (mod.audio) interface

The bundle includes a MOD GUI (`modgui.ttl` and `modgui/`). It is laid out like a pedal: two
rows of four knobs, an 8-position voice rotary with printed legends, a gate trim, a bypass LED
and a footswitch.

| File | Purpose |
|------|---------|
| `modgui.ttl` | Points MOD at the template, stylesheet, screenshot and thumbnail |
| `modgui/icon-jtbasssynth.html` | Pedal template (knobs, footswitch, LED, jacks) |
| `modgui/stylesheet-jtbasssynth.css` | Control positions and sprites |
| `modgui/*.png` | Panel artwork, knob film strips, screenshot, thumbnail |
| `tools/make_modgui.py` | Regenerates every PNG (`make modgui-images`, needs Pillow) |

If you change the layout in `tools/make_modgui.py`, update the matching `left`/`top`
values in the stylesheet.

**Building for a MOD device:** MOD units run on aarch64 Linux. Build the `.so` with
[mod-plugin-builder](https://github.com/moddevices/mod-plugin-builder) for your device
(for example `moddwarf`), or with an aarch64 cross-compiler. Then copy the whole
`JTBassSynth.lv2` folder to the device:

```bash
scp -r JTBassSynth.lv2 root@192.168.51.1:/root/.lv2/
```

Restart the MOD service (or reboot), and it will appear under *JT → Bass Synth*.
For desktop previews, the plugin works in [MOD Desktop](https://mod.audio/desktop/) using a
native build.

### Playing tips

* Tracking works anywhere on the neck (about 24–520 Hz) and is most reliable on single notes.
  A round tone (neck pickup, plucking over the neck) helps, because the tracker locks onto the
  fundamental. Low notes respond a little slower: the synth stays silent until it has the
  pitch. Measured with `make test`, that averages about 53 ms on the B string, 43 ms on E,
  36 ms on A, 32 ms on D and 28 ms on G (add 3–6 ms for the audio buffer).
* For classic squelchy synth bass, try *Saw + Sub*, Filter around 200 Hz, Resonance 0.6,
  Sweep 0.7 and Decay 250 ms.
* For a slow pad-like swell, try *PWM*, Attack 600 ms, Sweep 0.2 and Glide 60 ms.
