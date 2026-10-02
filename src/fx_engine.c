/* Vibe FX -- a multi-effect insert for MPC OS, built on this project's own effect cores
 * (the same reverb/delay/saturation/chorus that ship in the Mutable Vibe instrument).
 *
 * Fixed, musical topology (not freely routable, by design -- keeps it to one insert slot):
 *
 *     IN -> DRIVE -> CHORUS -> (dry) ------------------+
 *                               +-> REVERB x send ----+
 *                               +-> DELAY  x send ----+--> OUT
 *
 * Drive + chorus are a series front-end (shape the tone). Reverb and delay are independent parallel
 * sends off the post-chorus signal, each with its own send level, summed back with the dry. The
 * effect cores run in float interleaved stereo; the engine contract is int16, so we convert per block.
 * Contract: 44100 Hz, 128-frame, interleaved int16 stereo. */
#include "engine.h"   /* mpc-vst-plugins wrapper; the port builder puts wrapper/ on the include path */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* --- Mutable Vibe effect cores (vendored via vst.json sources; same decls as the synth port) --- */
void  *reverb_create(int sr);
void   reverb_free(void *r);
void   reverb_set_decay(void *r, float v);
void   reverb_set_damping(void *r, float v);
void   reverb_set_predelay(void *r, float ms);
void   reverb_set_wet(void *r, float v);
void   reverb_set_dry(void *r, float v);
void   reverb_set_hipass(void *r, float v);
void   reverb_process(void *r, float *in, float *out, int n_frames);

void  *delay_create(int samplerate);
void   delay_free(void *d);
void   delay_set_time_ms(void *d, float v);
void   delay_set_feedback(void *d, float v);
void   delay_set_tone(void *d, float v);
void   delay_set_hipass(void *d, float v);   /* high-pass in the feedback path: tames runaway low end */
void   delay_set_head_bump(void *d, float v);/* 60 Hz peak EQ in the feedback; default 0.4 can run the bass away */
void   delay_flush(void *d);                 /* clear the delay buffer (kill the tail) */
void   delay_process(void *d, const float *in, float *out, int n);

void  *chorus_create(float samplerate);
void   chorus_free(void *ptr);
void   chorus_set_rate(void *ptr, float hz);
void   chorus_set_depth(void *ptr, float d);
void   chorus_set_wet(void *ptr, float w);
void   chorus_process(void *ptr, float *in, float *out, int frames);

void  *sat_create(float samplerate);
void   sat_free(void *ptr);
void   sat_set_amount(void *ptr, float v);
void   sat_process(void *ptr, float *buf, int frames);

#define SR 44100

/* tempo-sync divisions for the delay (beats per repeat). Straight + dotted; the very long 2/4-bar
 * values are dropped (too long to be musical for a delay). Dotted = 1.5x (e.g. 1/8. = 0.75 beat). */
static const float       kDivBeats[7]  = { 0.25f, 0.5f, 0.75f, 1.f, 1.5f, 2.f, 4.f };
static const char *const kDivLabels[7] = { "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1" };

typedef struct {
    void *sat, *chorus, *reverb, *delay;
    /* cached param values (for get_param / state) */
    float drive;
    float chorusRate, chorusDepth, chorusMix;
    float reverbDecay, reverbDamping, reverbHipass, reverbPredelay, reverbSend;
    float delayTime, delayFeedback, delayTone, delaySend;   /* delayTone is bipolar: <0.5 LP, >0.5 HP */
    int   delaySync;      /* 0 free, 1 tempo-synced */
    int   delayDiv;       /* index into kDivBeats */
    float bpm;            /* host tempo via "lfo_bpm" (HAS_LFO_BPM) */
    float delayTimeCur;   /* last delay time pushed (ms), only re-set on change (no zipper) */
} fx_t;

/* The one-pole HP in both cores is coef = 1 - v*0.98, i.e. cutoff ~ LINEAR in v (~v*6880 Hz) -- so the
 * whole musical range bunches into the first ~15% of the knob. Map the knob to a LOG frequency instead
 * (25 Hz .. 2500 Hz) so each increment is a constant interval, then invert back to the core's v. */
static float hp_warp(float u) {
    if (u <= 0.001f) return 0.0f;                              /* off */
    float fc = 25.0f * powf(100.0f, u);                        /* 25 Hz (u=0) .. 2500 Hz (u=1), log-spaced */
    float v  = fc * 6.2831853f / (0.98f * (float)SR);          /* invert fc ~= v*0.98*SR/(2*pi) */
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Feedback safety curve. The knob shows 0..125. Below 100 it stays clearly decaying (normal echoes);
 * 100..125 are the long, near-self-oscillating tails. The coefficient is hard-capped at 0.98 so the
 * delay ALWAYS decays when the input stops -- it can never run away and hurt ears/speakers even when
 * you leave the plugin page. (Paired with head_bump = 0 in create(), which removes a 60 Hz feedback
 * boost that otherwise piled up the low end at feedback settings well under 50%.) */
static float fb_curve(float disp) {
    /* The tape soft-clip in the feedback (x*(1.5-0.5x^2)) has a 1.5x gain for small signals, so the real
     * loop gain is feedback*1.5. Cap the coefficient at 0.62 => loop gain ~0.93 < 1, i.e. the delay ALWAYS
     * decays (long musical tails at the top, but never self-oscillates or runs away). Display is 0..100. */
    float c = (disp / 100.0f) * 0.62f;
    return c < 0.0f ? 0.0f : (c > 0.62f ? 0.62f : c);
}

/* delayTone is one bipolar knob: 0.5 = neutral, left half a low-pass (dark), right half a feedback
 * high-pass (thins the runaway low end). Center does nothing. */
static void apply_delay_tone(fx_t *s) {
    float t = s->delayTone;
    if (t < 0.5f) { delay_set_tone(s->delay, t * 2.0f); delay_set_hipass(s->delay, 0.0f); }   /* LP side */
    else          { delay_set_tone(s->delay, 1.0f); delay_set_hipass(s->delay, hp_warp((t - 0.5f) * 2.0f)); }  /* HP side */
}

static void *create(const char *data_dir) {
    (void)data_dir;
    fx_t *s = (fx_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->sat    = sat_create((float)SR);
    s->chorus = chorus_create((float)SR);
    s->reverb = reverb_create(SR);
    s->delay  = delay_create(SR);
    if (!s->sat || !s->chorus || !s->reverb || !s->delay) return s;  /* leave partially built; hosts tolerate */

    /* reverb + delay are parallel *sends*: their cores output pure wet, the engine scales the mix. */
    reverb_set_dry(s->reverb, 0.0f);
    reverb_set_wet(s->reverb, 1.0f);

    /* defaults: clean-ish passthrough (everything down) so inserting it does nothing until dialed in */
    s->drive = 0.0f;
    s->chorusRate = 0.5f; s->chorusDepth = 0.5f; s->chorusMix = 0.0f;
    s->reverbDecay = 0.60f; s->reverbDamping = 0.15f; s->reverbHipass = 0.12f; s->reverbPredelay = 0.0f; s->reverbSend = 0.0f;
    s->delayTime = 300.0f; s->delayFeedback = 40.0f; s->delayTone = 0.5f; s->delaySend = 0.0f;  /* feedback shown 0..125 */
    s->delaySync = 1; s->delayDiv = 3; s->bpm = 120.0f; s->delayTimeCur = s->delayTime;   /* default: synced, 1/4 */

    sat_set_amount(s->sat, s->drive);
    chorus_set_rate(s->chorus, s->chorusRate);
    chorus_set_depth(s->chorus, s->chorusDepth);
    chorus_set_wet(s->chorus, s->chorusMix);
    reverb_set_decay(s->reverb, s->reverbDecay);
    reverb_set_damping(s->reverb, s->reverbDamping);
    reverb_set_hipass(s->reverb, hp_warp(s->reverbHipass));
    reverb_set_predelay(s->reverb, s->reverbPredelay);
    delay_set_head_bump(s->delay, 0.0f);   /* safety: kill the 60 Hz feedback boost (see fb_curve) */
    delay_set_time_ms(s->delay, s->delayTime);
    delay_set_feedback(s->delay, fb_curve(s->delayFeedback));
    apply_delay_tone(s);
    return s;
}

static void destroy(void *inst) {
    fx_t *s = (fx_t *)inst;
    if (!s) return;
    if (s->sat) sat_free(s->sat);
    if (s->chorus) chorus_free(s->chorus);
    if (s->reverb) reverb_free(s->reverb);
    if (s->delay) delay_free(s->delay);
    free(s);
}

static void midi(void *inst, const uint8_t *msg, int len) {
    fx_t *s = (fx_t *)inst;
    if (len < 1 || !s->delay) return;
    /* If MPC ever routes MIDI panic to an insert (it currently does not), flush on it too. */
    int all_off = ((msg[0] & 0xF0) == 0xB0 && len >= 2 && (msg[1] == 120 || msg[1] == 123));
    if (all_off || msg[0] == 0xFC) delay_flush(s->delay);
}

static void render(void *inst, int16_t *out_lr, int frames) {   /* effect: never called; emit silence */
    (void)inst; memset(out_lr, 0, sizeof(int16_t) * frames * 2);
}

static int16_t clamp_s16(float x) {
    x *= 32767.0f;
    return x < -32768.0f ? -32768 : x > 32767.0f ? 32767 : (int16_t)x;
}

static void process(void *inst, const int16_t *in, int16_t *out, int frames) {
    fx_t *s = (fx_t *)inst;
    float buf[256], tmp[256], rv[256], dl[256];   /* frames is 128 -> 256 interleaved samples */
    int n2 = frames * 2;
    for (int i = 0; i < n2; i++) buf[i] = in[i] * (1.0f / 32768.0f);

    /* delay time: tempo-synced from the host BPM, or the free ms value. Only re-set on change (no zipper). */
    float dlyTarget = s->delaySync
        ? kDivBeats[s->delayDiv] * 60000.0f / (s->bpm > 1.0f ? s->bpm : 120.0f)
        : s->delayTime;
    if (dlyTarget < 10.0f) dlyTarget = 10.0f;
    if (dlyTarget > 2000.0f) dlyTarget = 2000.0f;
    if (fabsf(dlyTarget - s->delayTimeCur) > 0.01f) { delay_set_time_ms(s->delay, dlyTarget); s->delayTimeCur = dlyTarget; }

    sat_process(s->sat, buf, frames);            /* DRIVE (in place) */
    chorus_process(s->chorus, buf, tmp, frames); /* CHORUS: buf -> tmp */
    memcpy(buf, tmp, (size_t)n2 * sizeof(float));/* buf = post drive+chorus (the dry sent onward) */

    reverb_process(s->reverb, buf, rv, frames);  /* parallel wet reverb */
    delay_process(s->delay, buf, dl, frames);    /* parallel wet delay */

    for (int i = 0; i < n2; i++)
        out[i] = clamp_s16(buf[i] + s->reverbSend * rv[i] + s->delaySend * dl[i]);
}

static void set_param(void *inst, const char *key, const char *val) {
    fx_t *s = (fx_t *)inst;
    if (!strcmp(key, "state")) {
        sscanf(val, "drive=%f;chorusRate=%f;chorusDepth=%f;chorusMix=%f;"
                    "reverbDecay=%f;reverbDamping=%f;reverbHipass=%f;reverbPredelay=%f;reverbSend=%f;"
                    "delayTime=%f;delayFeedback=%f;delayTone=%f;delaySend=%f;delaySync=%d;delayDiv=%d",
               &s->drive, &s->chorusRate, &s->chorusDepth, &s->chorusMix,
               &s->reverbDecay, &s->reverbDamping, &s->reverbHipass, &s->reverbPredelay, &s->reverbSend,
               &s->delayTime, &s->delayFeedback, &s->delayTone, &s->delaySend, &s->delaySync, &s->delayDiv);
        sat_set_amount(s->sat, s->drive);
        chorus_set_rate(s->chorus, s->chorusRate); chorus_set_depth(s->chorus, s->chorusDepth); chorus_set_wet(s->chorus, s->chorusMix);
        reverb_set_decay(s->reverb, s->reverbDecay); reverb_set_damping(s->reverb, s->reverbDamping);
        reverb_set_hipass(s->reverb, hp_warp(s->reverbHipass)); reverb_set_predelay(s->reverb, s->reverbPredelay);
        delay_set_feedback(s->delay, fb_curve(s->delayFeedback)); apply_delay_tone(s);
        s->delayTimeCur = -1.0f;   /* force process() to re-apply the delay time */
        return;
    }
    float v = (float)atof(val);
    if      (!strcmp(key, "drive"))          { s->drive = v;          sat_set_amount(s->sat, v); }
    else if (!strcmp(key, "chorusRate"))     { s->chorusRate = v;     chorus_set_rate(s->chorus, v); }
    else if (!strcmp(key, "chorusDepth"))    { s->chorusDepth = v;    chorus_set_depth(s->chorus, v); }
    else if (!strcmp(key, "chorusMix"))      { s->chorusMix = v;      chorus_set_wet(s->chorus, v); }
    else if (!strcmp(key, "reverbDecay"))    { s->reverbDecay = v;    reverb_set_decay(s->reverb, v); }
    else if (!strcmp(key, "reverbDamping"))  { s->reverbDamping = v;  reverb_set_damping(s->reverb, v); }
    else if (!strcmp(key, "reverbHipass"))   { s->reverbHipass = v;   reverb_set_hipass(s->reverb, hp_warp(v)); }
    else if (!strcmp(key, "reverbPredelay")) { s->reverbPredelay = v; reverb_set_predelay(s->reverb, v); }
    else if (!strcmp(key, "reverbSend"))     { s->reverbSend = v; }
    else if (!strcmp(key, "delayTime"))      { s->delayTime = v; }                        /* applied in process() */
    else if (!strcmp(key, "delayFeedback"))  { s->delayFeedback = v;  delay_set_feedback(s->delay, fb_curve(v)); }
    else if (!strcmp(key, "delayTone"))      { s->delayTone = v;      apply_delay_tone(s); }   /* bipolar LP/HP */
    else if (!strcmp(key, "delaySend"))      { s->delaySend = v; }
    else if (!strcmp(key, "delaySync"))      { s->delaySync = (v >= 0.5f) ? 1 : 0; }
    else if (!strcmp(key, "delayDiv"))       { int d = (int)(v + 0.5f); s->delayDiv = d < 0 ? 0 : (d > 6 ? 6 : d); }
    else if (!strcmp(key, "lfo_bpm"))        { if (v > 1.0f) s->bpm = v; }                 /* host tempo from wrapper */
}

static int get_param(void *inst, const char *key, char *buf, int len) {
    fx_t *s = (fx_t *)inst;
    if (!strcmp(key, "state"))
        return snprintf(buf, len, "drive=%g;chorusRate=%g;chorusDepth=%g;chorusMix=%g;"
                        "reverbDecay=%g;reverbDamping=%g;reverbHipass=%g;reverbPredelay=%g;reverbSend=%g;"
                        "delayTime=%g;delayFeedback=%g;delayTone=%g;delaySend=%g;delaySync=%d;delayDiv=%d",
                        s->drive, s->chorusRate, s->chorusDepth, s->chorusMix,
                        s->reverbDecay, s->reverbDamping, s->reverbHipass, s->reverbPredelay, s->reverbSend,
                        s->delayTime, s->delayFeedback, s->delayTone, s->delaySend, s->delaySync, s->delayDiv);
    if (!strcmp(key, "delayTime")) {   /* string display: synced -> "500 ms (1/4)", free -> "300 ms" (ms first: knob norm stays sane) */
        if (s->delaySync) {
            float ms = kDivBeats[s->delayDiv] * 60000.0f / (s->bpm > 1.0f ? s->bpm : 120.0f);
            return snprintf(buf, len, "%.0f ms (%s)", ms, kDivLabels[s->delayDiv]);
        }
        return snprintf(buf, len, "%.0f ms", s->delayTime);
    }
    if (!strcmp(key, "delaySync")) return snprintf(buf, len, "%d", s->delaySync);
    if (!strcmp(key, "delayDiv"))  return snprintf(buf, len, "%d", s->delayDiv);
    float v = 0; int found = 1;
    if      (!strcmp(key, "drive"))          v = s->drive;
    else if (!strcmp(key, "chorusRate"))     v = s->chorusRate;
    else if (!strcmp(key, "chorusDepth"))    v = s->chorusDepth;
    else if (!strcmp(key, "chorusMix"))      v = s->chorusMix;
    else if (!strcmp(key, "reverbDecay"))    v = s->reverbDecay;
    else if (!strcmp(key, "reverbDamping"))  v = s->reverbDamping;
    else if (!strcmp(key, "reverbHipass"))   v = s->reverbHipass;
    else if (!strcmp(key, "reverbPredelay")) v = s->reverbPredelay;
    else if (!strcmp(key, "reverbSend"))     v = s->reverbSend;
    else if (!strcmp(key, "delayFeedback"))  v = s->delayFeedback;
    else if (!strcmp(key, "delayTone"))      v = s->delayTone;
    else if (!strcmp(key, "delaySend"))      v = s->delaySend;
    else found = 0;
    if (found) return snprintf(buf, len, "%g", v);
    /* dynamic_display (wrapper asks "<key>_display"): delayTime shows the tempo-sync division (via the plain
     * "delayTime" case above); every other dynamic_display param is a 0..1 knob shown as a 0..100 percent. */
    {
        size_t klen = strlen(key);
        if (klen > 8 && !strcmp(key + klen - 8, "_display")) {
            char base[64];
            int bl = (int)(klen - 8);
            if (bl >= (int)sizeof base) bl = (int)sizeof base - 1;
            memcpy(base, key, bl);
            base[bl] = 0;
            if (!strcmp(base, "delayTime")) return get_param(inst, "delayTime", buf, len);
            char vb[32];
            if (get_param(inst, base, vb, sizeof vb) > 0)
                return snprintf(buf, len, "%.0f", atof(vb) * 100.0);
            return 0;
        }
    }
    return 0;
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, process };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
