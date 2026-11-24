/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * 6210 Drum Peter - DRUM page for FM1 Open (two-tone LCD edition).
 *
 * Included once by screen.c, after its drawing helpers and before
 * apply_controls(). Everything here runs on the control task and uses only
 * the public instrument API (instrument.h). The engine's own sequencer plays
 * the notes, so timing stays sample accurate.
 *
 * DRUM page, drum tracks (DRUM 808 or SAMPLER):
 *   16 white keys  steps of the selected drum (one page of 16)
 *   black keys     pick a drum and hear it (11 drums on 808, 8 on SAMPLER)
 *   OCT - / +      previous / next 16 steps
 *   SELECT         track 1..8        ALGORITHM  length 16/32/48/64
 *   PRESETS        kit / program     K1 drum, K2 velocity, K3 chance, K4 tune
 * DRUM page, melodic tracks (303, DX7, PSG, PLUCK):
 *   white keys toggle steps with the last played note; black keys play notes.
 *   K1 note, K2 velocity, K3 chance, K4 cutoff (303/PSG/PLUCK) or volume (DX7).
 * EDIT held: SAVE loads the 6210 kit (T1 808, T2 909 style, T3 303),
 *            PRESETS changes the track's engine. A short EDIT tap opens the
 *            sound editor as before.
 */
#include "drum808.h"

enum { DP_C_HEAD, DP_C_TITLE, DP_C_ROW, DP_C_BOX = DP_C_ROW + 11, DP_C_HINT, DP_C_REST, DP_C_COUNT };
_Static_assert(DP_C_COUNT <= 16, "DRUM page cache slots");

static unsigned dp_edit_used;
static uint32_t dp_note_at;
static const char *dp_note_top, *dp_note_mid;
static uint8_t dp_audition[GROOVE_PADS];
static unsigned dp_setup_checked;
static uint32_t dp_step_at, dp_step_ms = 115;
static unsigned dp_step_seen = 255;

static void dp_init(void)
{
    dp_edit_down = dp_edit_used = 0;
    dp_note_at = 0;
    memset(dp_audition, 0xff, sizeof dp_audition);
    dp_setup_checked = 0;
}
static void dp_note(const char *top, const char *mid, uint32_t now)
{
    dp_note_top = top; dp_note_mid = mid;
    dp_note_at = now | 1U;
}
static unsigned dp_pads(void) { return groove_pad_count(track); }
static const char *dp_source_short(unsigned type)
{
    static const char *const names[] = {"DX7", "303", "808", "PSG", "PLK", "SMP"};
    return type < GROOVE_SOURCES ? names[type] : "---";
}
static unsigned dp_black(unsigned key)
{
    /* Black keys in panel order; returns 0..10 or 255 for a white key. */
    for (unsigned i = 0; i < 10; ++i) if (edit_keys[i] == key) return i;
    return key == 25 ? 10U : 255U;
}

/* ---- The 6210 kit -------------------------------------------------------- */
/* Set one sound parameter, found by name, to an absolute value. */
static void dp_tone_set(unsigned t, unsigned part, const char *name, unsigned value)
{
    unsigned count = groove_tone_count(t, part);
    if (count > GROOVE_TONE_PARAMS) count = GROOVE_TONE_PARAMS;
    uint8_t values[GROOVE_TONE_PARAMS];
    if (groove_tone_copy(t, part, values) < 0) return;
    for (unsigned p = 0; p < count; ++p) {
        const fm1_param_t *d = groove_tone_parameter(t, part, p);
        if (!d || !d->name) continue;
        const char *a = d->name, *b = name;
        while (*a && *b && (*a | 32) == (*b | 32)) { ++a; ++b; }
        if (*a || *b) continue;
        if (value > d->max) value = d->max;
        if (value != values[p]) groove_tone_edit(t, part, p, (int)value - (int)values[p]);
        return;
    }
}
/* 909 voicing for T2: the same 808 engine set tighter, brighter and harder.
 * Each entry: part, parameter name, value. */
static const struct { uint8_t part; char name[7]; uint8_t value; } dp_909[] = {
    {D8_BD, "Decay", 46}, {D8_BD, "Tune", 84}, {D8_BD, "Attack", 96}, {D8_BD, "Tone", 74}, {D8_BD, "Drive", 28},
    {D8_SD, "Tone", 96}, {D8_SD, "Snappy", 112}, {D8_SD, "Tune", 82}, {D8_SD, "Decay", 74},
    {D8_CP, "Decay", 52}, {D8_CP, "Tune", 74}, {D8_CH, "Decay", 30}, {D8_CH, "Tune", 92},
    {D8_OH, "Decay", 58}, {D8_OH, "Tune", 92}, {D8_CY, "Tune", 84}, {D8_RS, "Decay", 30}, {D8_RS, "Tune", 80}};
/* Starter groove, one bit per step (bit 0 = step 1): track, pad, steps, accents. */
static const struct { uint8_t track, pad; uint16_t steps, accents; } dp_groove[] = {
    {0, D8_BD, 0x1111, 0x0101}, {0, D8_CH, 0x4444, 0}, {0, D8_RS, 0x4080, 0},
    {1, D8_CP, 0x1010, 0x1010}, {1, D8_OH, 0x4444, 0}, {1, D8_CH, 0x0101, 0}};
/* T1 DRUM 808, T2 DRUM 808 voiced like a 909, T3 BASS 303 with a short
 * acid line. Tracks 4..8 are left as they are. */
static int dp_kit_load(void)
{
    static const uint8_t engines[3] = {GROOVE_808, GROOVE_808, GROOVE_303};
    static const uint8_t acid[16] = {36, 0, 36, 48, 0, 39, 36, 0, 43, 0, 36, 46, 0, 39, 41, 0};
    for (unsigned t = 0; t < 3; ++t) {
        if (groove_source_type(t) != engines[t] && groove_source_set(t, engines[t])) return -1;
        groove_edit_clear(t, 0, 2);
        groove_sequence_length_set(t, 16);
    }
    for (unsigned i = 0; i < sizeof dp_909 / sizeof dp_909[0]; ++i)
        dp_tone_set(1, dp_909[i].part, dp_909[i].name, dp_909[i].value);
    for (unsigned i = 0; i < sizeof dp_groove / sizeof dp_groove[0]; ++i)
        for (unsigned s = 0; s < 16; ++s)
            if ((dp_groove[i].steps >> s) & 1U)
                groove_sequence_edit(dp_groove[i].track, s, groove_pad_note(dp_groove[i].track, dp_groove[i].pad),
                                     (dp_groove[i].accents >> s) & 1U ? 127U : 96U);
    for (unsigned s = 0; s < 16; ++s)
        if (acid[s]) groove_sequence_edit(2, s, acid[s], s % 4 == 3 || s == 0 ? 127U : 96U);
    return 0;
}
/* A fresh start has an empty project (no notes on any track). The first time
 * 6210 sees one it turns it into the drum kit; a project with notes in it is
 * never touched. Nothing is written to Flash until SAVE. */
static void dp_setup_once(void)
{
    if (dp_setup_checked || boot_active) return;
    dp_setup_checked = 1;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        struct groove_view v = {0};
        groove_get_view(t, 0, &v);
        if (v.active_steps) return;
    }
    if (!dp_kit_load()) {
        track = 0; drum = 0; cursor = 0;
        selected_note = groove_pad_note(0, 0);
        notice = "6210 KIT READY"; notice_until = fm1_millis() + 2500U;
    }
}

/* ---- Controls ------------------------------------------------------------ */
/* Runs early in apply_controls so EDIT/PLAY/SAVE are not taken by other
 * gestures. Returns 1 when the event was consumed. */
static int dp_edit_gesture(const struct fm1_controls *c, uint32_t pressed, uint32_t now,
                           unsigned *play_handled)
{
    if (page != DRUM) { dp_edit_down = 0; return 0; }
    if (!dp_edit_down) {
        if (!(pressed & BUTTON(EDIT)) || (c->buttons & ~BUTTON(EDIT))) return 0;
        dp_edit_down = 1;
        dp_edit_used = 0;
        return 1;
    }
    if (pressed & BUTTON(SAVE)) {
        dp_edit_used = 1;
        if (!release_keys()) return 1;
        groove_edit_checkpoint(0);
        int r = dp_kit_load();
        track = 0; drum = 0; cursor = 0; step_follow = 1;
        selected_note = groove_pad_note(0, 0);
        dp_note("6210 KIT", r ? "FAILED" : "LOADED", now);
        return 1;
    }
    if (pressed & BUTTON(PLAY)) { *play_handled = 1; dp_edit_used = 1; return 1; }
    if (!(c->buttons & BUTTON(EDIT))) {
        dp_edit_down = 0;
        if (dp_edit_used) return 1;
        /* A short tap keeps the original EDIT meaning: open the sound editor. */
        unsigned edit = groove_source_type(track) == GROOVE_DX7 ? FM_QUICK : TONE_EDIT;
        page = edit;
        advanced_edit = 0;
        if (page == TONE_EDIT) tone_pages[track][tone_slot()] = 0;
        if (page == TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
        return 1;
    }
    if (encoder_active(c->encoders) && !c->encoders[6]) dp_edit_used = 1;
    return 1;
}
static unsigned dp_tune_parameter(unsigned t, unsigned part)
{
    unsigned count = groove_tone_count(t, part);
    for (unsigned p = 0; p < count && p < GROOVE_TONE_PARAMS; ++p) {
        const fm1_param_t *d = groove_tone_parameter(t, part, p);
        if (d && d->name && (d->name[0] | 32) == 't' && (d->name[1] | 32) == 'u') return p;
    }
    return 0;
}
static void dp_controls(const struct fm1_controls *c, uint32_t pressed, uint32_t now)
{
    (void)now;
    select_track(c->encoders[0]);
    if (c->encoders[1]) sequence_length_adjust(c->encoders[1]);
    if (c->encoders[6]) groove_control_edit(track, c->encoders[6], 0, 0);
    struct groove_view v = {0};
    groove_get_view(track, cursor, &v);
    int paging = !!(pressed & BUTTON(OCT_UP)) - !!(pressed & BUTTON(OCT_DOWN));
    if (paging) {
        unsigned length = v.sequence_length ? v.sequence_length : 16U;
        unsigned first = step_first(&v);
        int next = (int)first + paging * 16;
        if (next >= 0 && next < (int)length) {
            cursor = (unsigned)next + (cursor & 15U);
            step_follow = v.playing && (unsigned)next == (v.play_step & ~15U);
        }
    }
    unsigned pads = dp_pads();
    if (c->encoders[2]) {
        if (pads) {
            drum = (unsigned)clamp((int)drum + c->encoders[2], 0, (int)pads - 1);
            selected_note = groove_pad_note(track, drum);
            drum_parts[track] = (uint8_t)drum;
        } else if (v.step.velocity) {
            unsigned note = (unsigned)clamp((int)v.step.note + c->encoders[2], 0, 127);
            groove_sequence_edit(track, cursor, note, v.step.velocity);
            selected_note = note;
        } else selected_note = (unsigned)clamp((int)selected_note + c->encoders[2], 0, 127);
    }
    if (c->encoders[3]) groove_step_setting_edit(track, cursor, 0, c->encoders[3]);
    if (c->encoders[4]) {
        /* CHANCE in eighths: 0 means always (8/8), 1..7 are 1/8..7/8. */
        int current = (int)groove_step_setting(track, cursor, 2);
        int eighths = clamp((current ? current : 8) + c->encoders[4], 1, 8);
        int target = eighths == 8 ? 0 : eighths;
        if (target != current) groove_step_setting_edit(track, cursor, 2, target - current);
    }
    if (c->encoders[5]) {
        int delta = fm_encoder_delta(5, c->encoders[5]);
        unsigned type = groove_source_type(track);
        if (pads) groove_tone_edit(track, drum, dp_tune_parameter(track, drum), delta);
        else if (type == GROOVE_DX7) groove_control_edit(track, 0, delta, 0);
        else groove_control_edit(track, 0, 0, delta);
    }
}
/* Keyboard on the DRUM page. Returns the keys that still play notes. */
static uint32_t dp_keys(uint32_t keys, uint32_t key_down, uint32_t held)
{
    struct groove_view v = {0};
    groove_get_view(track, cursor, &v);
    unsigned first = step_first(&v), pads = dp_pads();
    unsigned length = v.sequence_length ? v.sequence_length : 16U;
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t bit = 1U << white_keys[i];
        if ((key_down & keys) & bit) {
            if (first + i < length) toggle_step(first + i);
        }
        keys &= ~bit;
    }
    if (!pads) return keys;
    /* Black keys pick and play drums; their notes are handled here. */
    unsigned ch = groove_midi_input_channel(track);
    for (unsigned key = 0; key < 27; ++key) {
        unsigned b = dp_black(key);
        if (b == 255) continue;
        uint32_t bit = 1U << key;
        if (b < GROOVE_PADS && dp_audition[b] != 0xff && !(held & bit)) {
            post((uint8_t)(0x80 + ch), dp_audition[b], 0, 3);
            dp_audition[b] = 0xff;
        }
        if (b < pads && ((key_down & keys) & bit)) {
            drum = b;
            drum_parts[track] = (uint8_t)b;
            selected_note = groove_pad_note(track, b);
            if (dp_audition[b] == 0xff && selected_note < 128 &&
                post((uint8_t)(0x90 + ch), (uint8_t)selected_note, 110, 3))
                dp_audition[b] = (uint8_t)selected_note;
        }
        keys &= ~bit;
    }
    return keys;
}
static void dp_poll(uint32_t now)
{
    (void)now;
    dp_setup_once();
}

/* ---- Blink timing shared with the LEDs ------------------------------------ */
/* Phase within the current sixteenth, 0..255, from the last step change. */
static unsigned dp_phase(const struct groove_view *v)
{
    if (v->play_step != dp_step_seen) {
        if (dp_step_seen != 255 && frame_time - dp_step_at < 1000U && frame_time - dp_step_at > 20U)
            dp_step_ms = frame_time - dp_step_at;
        dp_step_seen = v->play_step;
        dp_step_at = frame_time;
    }
    if (v->tempo) dp_step_ms = 15000U / v->tempo;
    uint32_t since = frame_time - dp_step_at;
    return since >= dp_step_ms ? 255U : since * 256U / (dp_step_ms ? dp_step_ms : 1U);
}

/* ---- Drawing (240 x 240) -------------------------------------------------- */
static unsigned dp_box_active(void)
{
    return dp_edit_down || (dp_note_at && frame_time - dp_note_at < 1200U);
}
static void dp_draw_box(void)
{
    if (paint_rendering && (paint_top >= 176 || paint_top + PAINT_ROWS <= 48)) return;
    unsigned active = dp_box_active();
    uint32_t sig = active | (dp_edit_down << 1) | ((uintptr_t)dp_note_mid << 2);
    if (sig == cache->hlv[DP_C_BOX]) return;
    cache->hlv[DP_C_BOX] = sig;
    if (!active) return; /* Rows repaint themselves when the box closes. */
    paint_fill(20, 68, 200, 84, TEXT);
    frame(23, 71, 194, 78, BACK);
    text(36, 84, dp_edit_down ? "EDIT HELD" : dp_note_top, 1, BACK, TEXT);
    text(36, 100, dp_edit_down ? "SAVE" : dp_note_mid, 3, BACK, TEXT);
    if (dp_edit_down) text(36, 130, "LOADS THE 6210 KIT", 1, BACK, TEXT);
}
/* One row per drum, or a single NOTE row for a melodic track. */
static void dp_draw_rows(const struct groove_view *sel, unsigned first, unsigned box, unsigned phase)
{
    unsigned pads = dp_pads(), rows = pads ? pads : 1U, rh = pads > 8 ? 11U : pads ? 15U : 60U;
    unsigned length = sel->sequence_length ? sel->sequence_length : 16U;
    unsigned head = sel->playing && (sel->play_step & ~15U) == first ? (sel->play_step & 15U) : 16U;
    /* Clear what a taller grid of another track left under this one. */
    unsigned bottom = 48 + rows * rh;
    if (!(paint_rendering && (paint_top >= 172 || paint_top + PAINT_ROWS <= bottom)) &&
        cache->hlv[DP_C_REST] != (bottom | (box << 8))) {
        cache->hlv[DP_C_REST] = bottom | (box << 8);
        if (bottom < 172) paint_fill(0, bottom, 240, 172 - bottom, BACK);
    }
    for (unsigned r = 0; r < rows && r < 11; ++r) {
        unsigned y = 48 + r * rh;
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + rh <= paint_top)) continue;
        uint64_t lane = pads ? groove_drum_steps(track, r) : sel->active_steps;
        uint32_t bits = (uint32_t)((lane >> first) & 0xffffU);
        unsigned hit = head < 16U && ((bits >> head) & 1U) && phase < 110U && !sel->muted;
        uint32_t meta = head | ((r == drum) << 5) | (hit << 6) | (box << 7) | ((first / 16U) << 8) |
            ((length / 16U) << 11) | ((cursor - first < 16U ? cursor - first : 31U) << 14) | (rows << 19);
        uint32_t sig = signature(meta, &bits, sizeof bits);
        if (sig == cache->hlv[DP_C_ROW + r]) continue;
        cache->hlv[DP_C_ROW + r] = sig;
        paint_fill(0, y, 240, rh, BACK);
        /* Label: inverted for the selected drum, flashes on every hit. */
        unsigned lit = (r == drum) ^ hit;
        paint_fill(2, y, 30, rh - 2, lit ? TEXT : BACK);
        text(4, y + (rh - 9) / 2, pads ? groove_pad_label(track, r) : "NOTE", 1, lit ? BACK : TEXT, lit ? TEXT : BACK);
        for (unsigned i = 0; i < 16; ++i) {
            unsigned x = 36 + i * 12 + (i / 4) * 2, on = (bits >> i) & 1U;
            if (first + i >= length) frame(x, y, 10, rh - 2, PANEL);
            else if (i == head) {
                paint_fill(x, y, 10, rh - 2, TEXT);
                paint_fill(x + 2, y + 2, 6, rh - 6, on ? DIM : BACK);
            } else paint_fill(x, y, 10, rh - 2, on ? TEXT : PANEL);
            if (r == drum && first + i == cursor && i != head) paint_fill(x + 3, y + rh - 5, 4, 2, on ? BACK : TEXT);
        }
    }
}
static void dp_draw(const struct groove_view v[8])
{
    const struct groove_view *sel = &v[track];
    unsigned pads = dp_pads(), box = dp_box_active();
    unsigned phase = dp_phase(sel), first = step_first(sel);
    unsigned beat = sel->playing && (sel->play_step & 3U) == 0 && phase < 160U;
    unsigned length = sel->sequence_length ? sel->sequence_length : 16U;
    /* Header: track, engine chip, tempo, unsaved mark, beat square. */
    if (!(paint_rendering && paint_top >= 30)) {
        uint32_t sig = track | (sel->type << 3) | ((uint32_t)sel->tempo << 6) | ((uint32_t)beat << 16) |
            ((uint32_t)groove_project_dirty() << 17) | ((uint32_t)sel->muted << 18);
        if (sig != cache->hlv[DP_C_HEAD]) {
            cache->hlv[DP_C_HEAD] = sig;
            paint_fill(0, 0, 240, 27, BACK);
            text(6, 8, "T", 2, TEXT, BACK);
            number(18, 8, (int)track + 1, 1, 2, TEXT, BACK);
            paint_fill(34, 6, 38, 18, sel->muted ? DIM : TEXT);
            text(36, 8, dp_source_short(sel->type), 2, BACK, sel->muted ? DIM : TEXT);
            number(84, 8, sel->tempo, 3, 2, TEXT, BACK);
            text(122, 15, groove_project_dirty() ? "BPM *" : "BPM  ", 1, DIM, BACK);
            text(170, 8, "6210", 1, DIM, BACK);
            frame(220, 6, 16, 16, TEXT);
            paint_fill(223, 9, 10, 10, beat ? TEXT : BACK);
        }
    }
    /* Title: drum or track name, cursor step and the four 16-step pages. */
    if (!(paint_rendering && (paint_top >= 46 || paint_top + PAINT_ROWS <= 30))) {
        unsigned playing_page = sel->playing ? sel->play_step / 16U : 7U;
        uint32_t sig = drum | (track << 4) | ((first / 16U) << 7) | ((length / 16U) << 10) |
            (playing_page << 13) | ((uint32_t)sel->type << 17) | (cursor << 20);
        if (sig != cache->hlv[DP_C_TITLE]) {
            cache->hlv[DP_C_TITLE] = sig;
            paint_fill(0, 30, 240, 16, BACK);
            text(4, 34, pads ? groove_pad_name(track, drum) : sel->type == GROOVE_303 ? "ACID 303" : "STEPS", 1, TEXT, BACK);
            number(118, 34, (int)cursor + 1, 2, 1, DIM, BACK);
            for (unsigned p = 0; p < 4; ++p) {
                unsigned x = 156 + p * 20;
                if (p * 16 >= length) { frame(x, 33, 16, 9, PANEL); continue; }
                paint_fill(x, 33, 16, 9, p == first / 16 ? TEXT : PANEL);
                if (p == playing_page) paint_fill(x + 6, 36, 4, 3, p == first / 16 ? BACK : TEXT);
            }
        }
    }
    dp_draw_rows(sel, first, box, phase);
    dp_draw_box();
    /* K1..K4. */
    char value[12];
    struct groove_view cell = {0};
    groove_get_view(track, cursor, &cell);
    if (pads) strcpy(value, groove_pad_label(track, drum));
    else note_name(cell.step.velocity ? cell.step.note : selected_note, value);
    knob(0, pads ? "K1 DRUM" : "K1 NOTE", value, TEXT);
    decimal(value, (int)groove_step_setting(track, cursor, 0), 1);
    knob(1, "K2 VELO", value, TEXT);
    unsigned chance = groove_step_setting(track, cursor, 2);
    decimal(value, chance ? (int)((chance * 100U + 4U) / 8U) : 100, 1);
    strcat(value, "%");
    knob(2, "K3 CHANCE", value, TEXT);
    uint8_t values[GROOVE_TONE_PARAMS];
    unsigned dx7 = !pads && sel->type == GROOVE_DX7;
    if (dx7) decimal(value, sel->volume, 1);
    else if (groove_tone_copy(track, pads ? drum : 0, values) < 0) strcpy(value, "--");
    else decimal(value, values[pads ? dp_tune_parameter(track, drum) : 0], 1);
    knob(3, pads ? "K4 TUNE" : dx7 ? "K4 VOLUME" : "K4 CUTOFF", value, TEXT);
    if (!(paint_rendering && paint_top + PAINT_ROWS <= 229)) {
        const char *hint = notice ? notice : pads ? "WHITE STEPS  BLACK DRUMS  OCT PAGE" : "WHITE STEPS  BLACK NOTES  OCT PAGE";
        uint32_t sig = signature(0, hint, (unsigned)strlen(hint));
        if (sig != cache->hlv[DP_C_HINT]) {
            cache->hlv[DP_C_HINT] = sig;
            paint_fill(0, 229, 240, 11, BACK);
            text(4, 231, hint, 1, DIM, BACK);
        }
    }
}
