/* SPDX-License-Identifier: GPL-3.0-only */
/* Dinghy: the smallest FM-1 firmware to build your own on. A sine wave on the keys, and that is all.
 * This is the file to change. Everything below it (platform.c) is done: updates, the USB rescue,
 * MIDI (USB, the TRS jack), USB audio, keys, knobs, LEDs, the screen, flash storage; the FM-1 and the SMK-37.
 *
 *   keys       play sines, 4 voices; the notes go out over USB-MIDI too, on the MIDI channel
 *   OCT- OCT+  the octave (kept in flash)
 *   PRESETS    the MIDI channel: OMNI (in: all, out: 1) or 1..16 (kept in flash)
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes from USB or the TRS jack play the sines, with their velocity; a MIDI clock shows its tempo
 *              and blinks PLAY on the beat, START / STOP light it
 *   SMK-37     the keys with velocity, the outer keys too, the pads (white keys F3..G5, lit while they sound), the
 *              pitch wheel bends +-2 semitones
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
static struct { uint8_t oct, ch; uint8_t space, dub, motion, color, arp, chord; } set = {0, 0, 72, 68, 42, 48, 0, 0};

#define NV 4u                                   /* voices */
static struct {
    volatile uint32_t inc;                      /* phase step per frame: f * 2^32 / 44100 (unbent) */
    volatile int32_t gate;                      /* target level, Q15: 0 = released */
    int32_t amp;                                /* the level now: glides to gate, no clicks */
    uint32_t ph, age;
    uint8_t note;
} voice[NV];
static volatile uint32_t bend_q16 = 65536;      /* the pitch wheel: the steps times this / 65536 */

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

/* Atmosphere FX: memory-bounded integer DSP for AC791N/pi32v2.
 * DUB is a mono feedback delay mixed back to stereo; SPACE is a pair of
 * short, cross-fed diffusion lines. No allocation, float, division or flash
 * access happens in the audio ISR. */
#define DUB_N 16384u
#define DUB_MASK (DUB_N - 1u)
#define RV1_N 1877u
#define RV2_N 2411u
static int16_t dub_mem[DUB_N];
static int16_t rv1_mem[RV1_N], rv2_mem[RV2_N];
static uint32_t dub_wp, rv1_wp, rv2_wp;
static int32_t color_lp_l, color_lp_r;

static int32_t sat16(int32_t x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

static void atmosphere_fx(int32_t *out, uint32_t n)
{
    uint32_t i;
    /* 92..372 ms at 44.1 kHz; DUB also raises feedback/wet. */
    uint32_t dly = 4096u + ((uint32_t)set.motion * 123u); /* <= 16396, clamped */
    uint32_t dub_wet = ((uint32_t)set.dub * 118u);       /* Q15, max ~0.36 */
    uint32_t dub_fb  = 8192u + ((uint32_t)set.dub * 150u); /* 0.25..0.71 */
    uint32_t sp_wet  = ((uint32_t)set.space * 105u);     /* max ~0.32 */
    uint32_t sp_fb   = 9000u + ((uint32_t)set.space * 105u);
    if (dly >= DUB_N) dly = DUB_N - 1u;

    for (i = 0; i < n; i++) {
        int32_t l = out[2u*i], r = out[2u*i+1u];
        int32_t mono = (l + r) >> 1;
        uint32_t rp = (dub_wp - dly) & DUB_MASK;
        int32_t dd = dub_mem[rp];
        int32_t feed = mono + ((dd * (int32_t)dub_fb) >> 15);
        dub_mem[dub_wp] = (int16_t)sat16(feed);
        dub_wp = (dub_wp + 1u) & DUB_MASK;

        /* Two unequal lines create a cheap diffuse stereo tail. */
        int32_t a = rv1_mem[rv1_wp], b = rv2_mem[rv2_wp];
        int32_t rin1 = mono + ((b * (int32_t)sp_fb) >> 15);
        int32_t rin2 = mono - ((a * (int32_t)sp_fb) >> 15);
        rv1_mem[rv1_wp] = (int16_t)sat16(rin1);
        rv2_mem[rv2_wp] = (int16_t)sat16(rin2);
        if (++rv1_wp == RV1_N) rv1_wp = 0;
        if (++rv2_wp == RV2_N) rv2_wp = 0;

        l += (dd * (int32_t)dub_wet) >> 15;
        r += (dd * (int32_t)dub_wet) >> 15;
        l += (a * (int32_t)sp_wet) >> 15;
        r += (b * (int32_t)sp_wet) >> 15;

        /* COLOR: one-pole dark/bright filter followed by gentle tape-like drive.
         * 0 = dark and soft, 50 = neutral-ish, 100 = bright with more saturation. */
        {
            int32_t cf = 1400 + (int32_t)set.color * 250; /* Q15 coefficient */
            int32_t drive = 32768 + (int32_t)set.color * 95;
            color_lp_l += ((l - color_lp_l) * cf) >> 15;
            color_lp_r += ((r - color_lp_r) * cf) >> 15;
            l = (color_lp_l * drive) >> 15;
            r = (color_lp_r * drive) >> 15;
            /* soft knee without division/float */
            if (l > 24576) l = 24576 + ((l - 24576) >> 2);
            if (l < -24576) l = -24576 + ((l + 24576) >> 2);
            if (r > 24576) r = 24576 + ((r - 24576) >> 2);
            if (r < -24576) r = -24576 + ((r + 24576) >> 2);
        }
        out[2u*i]   = sat16(l);
        out[2u*i+1u]= sat16(r);
    }
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t v, i;
    for (v = 0; v < NV; v++) {
        uint32_t ph = voice[v].ph, step = (uint32_t)(((uint64_t)voice[v].inc * bend_q16) >> 16);
        int32_t amp = voice[v].amp, g = voice[v].gate;
        if (!amp && !g) continue;
        for (i = 0; i < n; i++) {
            int32_t smp;
            if (amp < g) { amp += 48; if (amp > g) amp = g; }
            else if (amp > g) { amp -= 6; if (amp < g) amp = g; }
            ph += step;
            smp = (sine(ph) * amp) >> 15;
            out[2u*i] += smp;
            out[2u*i+1u] += smp;
        }
        voice[v].ph = ph; voice[v].amp = amp;
    }
    atmosphere_fx(out, n);
}

/* ---- notes (main loop) */
static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t ages, last_note = 255;

static void play(uint32_t note, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    if (note > 127u || !vel) return;
    for (v = 0; v < NV; v++) {                  /* the same note, else a free voice, else the oldest */
        uint32_t score = voice[v].note == note && voice[v].gate ? 0u
                       : voice[v].gate ? 0x80000000u + voice[v].age : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    voice[pick].note = (uint8_t)note;
    voice[pick].age = ++ages;
    voice[pick].inc = INC_C9[note % 12u] >> (10u - note / 12u);
    voice[pick].gate = (int32_t)(vel * 90u);    /* 127 -> 11430: four voices stay under full scale */
    last_note = note;
}
static void stop(uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].note == note)
            voice[v].gate = 0;
}
static void chord_play(uint32_t note, uint32_t vel)
{
    play(note, vel);
    if (note + 7u < 128u) play(note + 7u, vel > 12u ? vel - 12u : vel);
    if (note + 12u < 128u) play(note + 12u, vel > 20u ? vel - 20u : vel);
}
static void chord_stop(uint32_t note)
{
    stop(note);
    if (note + 7u < 128u) stop(note + 7u);
    if (note + 12u < 128u) stop(note + 12u);
}
static int quiet(void)                          /* nothing sounds, not even a release */
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].gate || voice[v].amp) return 0;
    return 1;
}


/* ---- the screen: the name, the last note big, the keyboard with what sounds, the octave */
#define C_ACC RGB(184, 196, 62)                /* lilac, as the logo */
#define C_KEY RGB(168, 174, 118)
#define C_OFF RGB(112, 120, 74)
#define KEY0 53u                                /* key 0 = F3 */
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static void draw_note(void)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char s[8] = "-";
    if (last_note < 128u) {
        const char *p = NAME[last_note % 12u];
        uint32_t i = 0;
        while (*p) s[i++] = *p++;
        if (last_note >= 12u) s[i++] = (char)('0' + last_note / 12u - 1u);
        s[i] = 0;
    }
    draw_text_box(0, 82, 240, &FONT_L, s, C_WHITE, 1);
}

static void draw_keys(uint32_t lit)             /* bit k: key k sounds */
{
    uint32_t k, w = 0;
    cv_begin(240, 56, C_BLACK);
    for (k = 0; k < 27u; k++)                   /* 16 white keys of 14 px from x = 8 */
        if (!black(KEY0 + k)) {
            cv_rect(8 + (int32_t)w * 14, 0, 13, 56, lit >> k & 1u ? C_ACC : C_KEY);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)            /* the black ones on the line between two white */
        if (black(KEY0 + k))
            cv_rect(8 + (int32_t)w * 14 - 5, 0, 9, 34, lit >> k & 1u ? C_ACC : C_BLACK);
        else
            w++;
    cv_blit(0, 140);
}

static void draw_status(int32_t oct, uint32_t ch)
{
    char s[24] = "OCT +0  MIDI OMNI";
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    if (ch) {
        s[13] = 'C'; s[14] = 'H'; s[15] = (char)(ch < 10u ? ' ' : '1'); s[16] = (char)('0' + ch % 10u);
    }
    draw_text_box(0, 222, 240, &FONT_S, s, C_OFF, 1);
}

static void draw_macros(void)
{
    char s[48] = "SPACE 00  DUB 00  MOT 00  COL 00";
    uint8_t v[4] = {set.space, set.dub, set.motion, set.color};
    uint8_t pos[4] = {6, 14, 22, 30};
    uint32_t i;
    for (i=0;i<4;i++) { s[pos[i]]=(char)('0'+v[i]/10); s[pos[i]+1]=(char)('0'+v[i]%10); }
    draw_text_box(0, 190, 240, &FONT_S, s, C_ACC, 1);
    draw_text_box(0, 202, 240, &FONT_S, set.arp ? (set.chord ? "ARP ON   CHORD ON" : "ARP ON   CHORD OFF") : (set.chord ? "ARP OFF  CHORD ON" : "ARP OFF  CHORD OFF"), C_OFF, 1);
}

static void draw_midi(uint32_t src, uint32_t bpm, uint32_t running)   /* where notes came from last, the clock */
{
    char s[32] = "", *p = s;
    const char *a = src == 2u ? "TRS IN" : src == 1u ? "USB IN" : "";
    while (*a) *p++ = *a++;
    if (bpm) {
        if (p != s) { *p++ = ' '; *p++ = ' '; }
        a = running ? "CLOCK " : "clock ";
        while (*a) *p++ = *a++;
        if (bpm >= 100u) *p++ = (char)('0' + bpm / 100u);
        *p++ = (char)('0' + bpm / 10u % 10u);
        *p++ = (char)('0' + bpm % 10u);
        a = " BPM";
        while (*a) *p++ = *a++;
    }
    *p = 0;
    draw_text_box(0, 120, 240, &FONT_S, s, C_OFF, 1);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
   /* what survives a power-off (add yours, up to SECTOR_MAX bytes) */
static uint32_t idle_ok, midi_src, clk_n, clk_t0, clk_bpm, clk_run, clk_beat_ms;

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {   /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) {                      /* clock: 24 a quarter note */
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;   /* a quarter's length */
                    clk_bpm = ms ? (60000u + ms / 2u) / ms : 0;
                }
                clk_t0 = fm1_ms;
                clk_beat_ms = fm1_ms;
            }
            clk_n++;
            continue;
        }
        if (st == 0xFAu) { clk_run = 1; clk_n = 0; continue; }   /* START: the next clock is beat 1 */
        if (st == 0xFBu) { clk_run = 1; continue; }              /* CONTINUE */
        if (st == 0xFCu) { clk_run = 0; continue; }              /* STOP */
        if (set.ch && (st & 15u) != set.ch - 1u) continue;      /* not our channel */
        if (ty == 0x90u && d2) { play(d1, d2); midi_src = (uint32_t)src; }
        else if (ty == 0x80u || ty == 0x90u) stop(d1);
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0, lit_drawn = ~0u, note_drawn = ~0u, status_drawn = ~0u;
    uint32_t midi_drawn = ~0u, macro_drawn = ~0u;
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    if (settings_load(&set, sizeof set) || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u) {
        set.oct = 0; set.ch = 0; set.space = 72; set.dub = 68; set.motion = 42; set.color = 48; set.arp = 0; set.chord = 0;
    }

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_L, APP_NAME, C_ACC, 1);
    draw_text_box(0, 52, 240, &FONT_S, "ATMOSPHERE FX  v" APP_VER, C_OFF, 1);

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, out_ch = set.ch ? set.ch - 1u : 0u, st;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {                 /* (a key stops the note it started: the octave may change meanwhile) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = 100;
#endif
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); if (!set.arp) { if (set.chord) chord_play(held[k], vel); else play(held[k], vel); } midi_send(0x90u | out_ch, held[k], vel); }
            if (up >> k & 1u) { if (!set.arp) { if (set.chord) chord_stop(held[k]); else stop(held[k]); } midi_send(0x80u | out_ch, held[k], 0); }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); play(ext_note[key], vel); midi_send(0x90u | out_ch, ext_note[key], vel); }
            else if (ext_note[key]) { stop(ext_note[key]); midi_send(0x80u | out_ch, ext_note[key], 0); ext_note[key] = 0; }
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            uint32_t w = pad < 8u ? pad + 8u : pad - 8u;   /* bottom row = white keys 1-8, top row 9-16 */
            if (vel) { pad_note[pad] = (uint8_t)(base + WHITE[w]); play(pad_note[pad], vel); midi_send(0x90u | out_ch, pad_note[pad], vel); }
            else if (pad_note[pad]) { stop(pad_note[pad]); midi_send(0x80u | out_ch, pad_note[pad], 0); pad_note[pad] = 0; }
        }
        {
            int32_t bd = smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms);   /* 1/4096 semitone */
            bend_q16 = (uint32_t)(65536 + (bd * 3785 >> 12) + (((bd * bd) >> 12) * 109 >> 12));   /* ~2^(s/12) */
        }
#endif

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {   /* detents turned since the last call */
            int32_t c = (int32_t)set.ch + e;
            set.ch = (uint8_t)(c < 0 ? 0 : c > 16 ? 16 : c);
            dirty = 1;
            t_change = fm1_ms;
        }
        if ((e = fm1_enc_take(ENC_KNOB1)) != 0) { int32_t x=(int32_t)set.space+e*2; set.space=(uint8_t)(x<0?0:x>100?100:x); dirty=1; t_change=fm1_ms; }
        if ((e = fm1_enc_take(ENC_KNOB2)) != 0) { int32_t x=(int32_t)set.dub+e*2; set.dub=(uint8_t)(x<0?0:x>100?100:x); dirty=1; t_change=fm1_ms; }
        if ((e = fm1_enc_take(ENC_KNOB3)) != 0) { int32_t x=(int32_t)set.motion+e*2; set.motion=(uint8_t)(x<0?0:x>100?100:x); dirty=1; t_change=fm1_ms; }
        if ((e = fm1_enc_take(ENC_KNOB4)) != 0) { int32_t x=(int32_t)set.color+e*2; set.color=(uint8_t)(x<0?0:x>100?100:x); dirty=1; t_change=fm1_ms; }
        if (b & (1u << BTN_ARP)) { set.arp ^= 1u; dirty=1; t_change=fm1_ms; }
        if (b & (1u << BTN_SEQ)) { set.chord ^= 1u; dirty=1; t_change=fm1_ms; }

        /* Simple clockable arpeggiator. MOTION sets 70..310 ms per step.
         * Held physical keys form the pattern; external MIDI still plays normally. */
        {
            static uint32_t arp_t, arp_i; static int32_t arp_note = -1;
            uint32_t period = 310u - (uint32_t)set.motion * 240u / 100u;
            if (!set.arp || !keys) { if (arp_note >= 0) { stop((uint32_t)arp_note); arp_note=-1; } }
            else if (fm1_ms - arp_t >= period) {
                uint32_t count=0, target, kk;
                if (arp_note >= 0) stop((uint32_t)arp_note);
                for (kk=0;kk<27u;kk++) if (keys >> kk & 1u) count++;
                if (count) {
                    target = arp_i++ % count; count=0;
                    for (kk=0;kk<27u;kk++) if (keys >> kk & 1u) { if (count++ == target) { arp_note=(int32_t)(base+kk); if (set.chord) chord_play((uint32_t)arp_note, 92); else play((uint32_t)arp_note, 100); break; } }
                }
                arp_t=fm1_ms;
            }
        }

        midi_in();
        if (clk_bpm && fm1_ms - clk_t0 > 2000u) clk_bpm = 0, clk_run = 0;   /* the clock has stopped coming */

        oct = (int8_t)set.oct;
        base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        for (k = 0; k < NV; k++)                /* what sounds, on the keys of this octave */
            if (voice[k].gate && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_PLAY, clk_run || (clk_bpm && fm1_ms - clk_beat_ms < 80u));   /* lit: running; a blink a beat */
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows lilac while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 90 : 4, on ? 60 : 2, on ? 128 : 8);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if (lit != lit_drawn) { lit_drawn = lit; draw_keys(lit); }   /* redraw only what changed */
        if (last_note != note_drawn) { note_drawn = last_note; draw_note(); }
        if ((st = (uint32_t)(oct + 2) | (uint32_t)set.ch << 4) != status_drawn) { status_drawn = st; draw_status(oct, set.ch); }
        if ((st = midi_src | clk_bpm << 2 | clk_run << 12) != midi_drawn) { midi_drawn = st; draw_midi(midi_src, clk_bpm, clk_run); }
        st = (uint32_t)set.space | (uint32_t)set.dub<<7 | (uint32_t)set.motion<<14 | (uint32_t)set.color<<21 | (uint32_t)set.arp<<28 | (uint32_t)set.chord<<29;
        if (st != macro_drawn) { macro_drawn = st; draw_macros(); }
        idle_ok = 0;

        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
