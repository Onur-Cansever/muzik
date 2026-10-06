/*
 * muzik.c - endless generative music to stdout
 *
 * Pure C, POSIX only. No audio library, no device code:
 * raw mono PCM (44100 Hz, 16-bit, little-endian) on fd 1.
 *
 * Build:  cc -O2 -o muzik muzik.c -lm
 * Run (Linux, ALSA):    ./muzik | aplay -f S16_LE -r 44100 -c 1
 * Run (FreeBSD, SoX):   ./muzik | play -t raw -e signed-integer -b 16 -r 44100 -c 1 -
 * Run (FFmpeg, both):   ./muzik | ffplay -nodisp -autoexit -f s16le -ar 44100 -i -
 * Args:   muzik [bpm] [seed]        (both optional)
 *
 * Pattern layers:
 *   key    : A minor pentatonic (scale[] below)
 *   tempo  : deterministic beat clock (bpm), no randomness
 *   rhythm : 16-step bar; kick / snare / hat / bass
 *   flow   : every 8 bars a new section rolls energy + density
 *   prng   : LCG; seed from clock when not given
 *
 * Stop with Ctrl-C in the aplay side; the pipe closes and
 * the writer exits on its next write.
 */
/* POSIX feature test: clock_gettime(CLOCK_MONOTONIC) in strict -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#define SR 44100
#define PI 3.14159265358979323846

/* ---------- PRNG (LCG) ---------- */
static uint64_t rng_state;
static uint64_t rng(void) {
    rng_state = rng_state * 6364136223846793005ULL
                       + 1442695040888963407ULL;
    return rng_state >> 11;
}
static double r01(void) {
    return (double)(rng() >> 11) / (double)(1ULL << 53);
}
static int pick(int n) {
    return (int)(rng() % (uint64_t)n);
}

/* ---------- key: A minor pentatonic, semitones above A3 ---------- */
static const int scale[] = { 0, 3, 5, 7, 10, 12, 15, 17, 19, 22 };
#define NSC ((int)(sizeof(scale) / sizeof(scale[0])))
static double hz(int semis) {
    return 220.0 * pow(2.0, semis / 12.0);
}

/* ---------- voice pool: plucked sines, exponential decay ---------- */
#define NV 16
static double vph[NV], vfr[NV], vamp[NV], vdec[NV];
static int    vact[NV];
static int    vnext;
static void note_on(double f, double amp, double dec) {
    int i = vnext;
    vnext = (vnext + 1) % NV;
    vph[i] = 0.0; vfr[i] = f; vamp[i] = amp; vdec[i] = dec; vact[i] = 1;
}

/* ---------- drum state ---------- */
static double kick_amp, kick_ph, kick_fr;
static double hat_amp;
static double snr_amp, snr_ph;

/* ---------- flow state ---------- */
static double energy, density;
static void new_section(void) {
    energy  = 0.40 + 0.60 * r01();
    density = 0.35 + 0.55 * r01();
}

/* ---------- melodic walk state ---------- */
static int mel_idx = 3;

/* ---------- scheduler: one 16th-note step (s = 0..15) ---------- */
static void step(int s) {
    if (s == 0 || s == 8) {                      /* kick: beats 1 & 3 */
        kick_amp = 0.55 * (0.70 + 0.30 * r01()) * energy;
        kick_fr  = 160.0;
    }
    if (s == 4 || s == 12) {                     /* snare: beats 2 & 4 */
        snr_amp = 0.30 * energy;
    }
    if (r01() < 0.75) {                          /* hat */
        hat_amp = (0.06 + 0.10 * r01()) * (0.5 + 0.5 * energy);
    }
    if ((s & 3) == 0) {                          /* bass on every beat */
        static const int bass_pat[4] = { 0, 0, 2, 3 };
        int bi = bass_pat[s >> 2];
        if (r01() < 0.15) bi += pick(3);         /* occasional spice */
        if (bi >= NSC) bi = NSC - 1;
        note_on(hz(scale[bi] - 12), 0.30 * (0.8 + 0.2 * r01()) * energy,
                0.9995);
    }
    if (r01() < density * (0.5 + 0.5 * energy)) { /* melody */
        static const int deltas[4] = { -2, -1, 1, 2 };
        mel_idx += deltas[pick(4)];
        if (mel_idx < 0) mel_idx = 0;
        if (mel_idx >= NSC) mel_idx = NSC - 1;
        note_on(hz(scale[mel_idx]), 0.22 * (0.7 + 0.3 * r01()) * energy,
                0.9992);
    }
}

/* ---------- per-sample synthesis ---------- */
static void sample(double *out) {
    double s = 0.0;
    int i;
    for (i = 0; i < NV; i++) {
        if (!vact[i]) continue;
        vph[i]  += vfr[i] / SR;
        vamp[i] *= vdec[i];
        if (vamp[i] < 0.0004) { vact[i] = 0; continue; }
        s += sin(2.0 * PI * vph[i]) * vamp[i];
    }
    if (kick_amp > 0.001) {                      /* pitch-dropping sine */
        kick_ph += kick_fr / SR;
        kick_fr *= 0.985;
        if (kick_fr < 40.0) kick_fr = 40.0;
        s += sin(2.0 * PI * kick_ph) * kick_amp;
        kick_amp *= 0.988;
    }
    if (hat_amp > 0.001) {                       /* noise burst */
        s += (r01() * 2.0 - 1.0) * hat_amp;
        hat_amp *= 0.82;
    }
    if (snr_amp > 0.001) {                       /* noise + 180 Hz body */
        snr_ph += 180.0 / SR;
        s += sin(2.0 * PI * snr_ph) * snr_amp * 0.4
           + (r01() * 2.0 - 1.0) * snr_amp;
        snr_amp *= 0.94;
    }
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    *out = s * 0.85;
}

/* ---------- per-sample output: mono ---------- */
static int16_t clamp16(double x) {
    if (x >  32767.0) x =  32767.0;
    if (x < -32768.0) x = -32768.0;
    return (int16_t)x;
}

int main(int argc, char **argv) {
    double bpm = 120.0;
    if (argc > 1) bpm = atof(argv[1]);
    if (bpm < 40.0)  bpm = 40.0;
    if (bpm > 240.0) bpm = 240.0;

    if (argc > 2) {
        rng_state = (uint64_t)atoll(argv[2]);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        rng_state = (uint64_t)ts.tv_sec * 1000000000ULL
                  + (uint64_t)ts.tv_nsec;
    }
    if (rng_state == 0) rng_state = 0x9e3779b97f4a7c15ULL;

    double beat  = 60.0 / bpm;
    double stepd = beat / 4.0;
    new_section();

    double t = 0.0, next = 0.0;
    int stepn = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double target = (double)t0.tv_sec + (double)t0.tv_nsec * 1e-9; /* realtime */

    enum { CH = 4410 };                          /* 0.1 s per block */
    int16_t buf[CH];

    for (;;) {
        int i;
        for (i = 0; i < CH; i++) {
            while (t >= next - 1e-9) {
                if (stepn > 0 && stepn % 128 == 0) new_section(); /* 8 bars */
                step(stepn % 16);
                stepn++;
                next += stepd;
            }
            double s;
            sample(&s);
            buf[i] = clamp16(s * 32767.0);
            t += 1.0 / SR;
        }
        if (write(STDOUT_FILENO, buf, sizeof buf) != (ssize_t)sizeof buf)
            return 1;                            /* pipe closed */
        /* Pace to realtime: sleep the remainder of this block's window.
         * Without this the writer runs at CPU speed; live playback only
         * survives via pipe backpressure, and file captures would be
         * thousands of seconds of audio per second of wall time. */
        target += (double)CH / SR;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double dt = target - ((double)now.tv_sec + (double)now.tv_nsec * 1e-9);
        if (dt > 0.0) {
            struct timespec req;
            req.tv_sec  = (time_t)dt;
            req.tv_nsec = (long)((dt - (double)req.tv_sec) * 1e9);
            nanosleep(&req, NULL);
        }
    }
}
