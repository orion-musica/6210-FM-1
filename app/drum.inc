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

enum { DP_C_HEAD, DP_C_TITLE, DP_C_ROW, DP_C_INFO = DP_C_ROW + 11, DP_C_BOX, DP_C_HINT, DP_C_COUNT };
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
    dp_note_at = now ? now : 1U;
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
static void dp_drum_line(unsigned t, unsigned pad, const char *pattern, unsigned velocity)
{
    for (unsigned s = 0; s < 16 && pattern[s]; ++s)
        if (pattern[s] != '.')
            groove_sequence_edit(t, s, groove_pad_note(t, pad), pattern[s] == 'X' ? 127U : velocity);
}
/* T1 DRUM 808, T2 DRUM 808 voiced like a 909 (shorter, brighter, harder),
 * T3 BASS 303 with a short acid line. Tracks 4..8 are left as they are. */
static int dp_kit_load(void)
{
    if (groove_source_type(0) != GROOVE_808 && groove_source_set(0, GROOVE_808)) return -1;
    if (groove_source_type(1) != GROOVE_808 && groove_source_set(1, GROOVE_808)) return -1;
    if (groove_source_type(2) != GROOVE_303 && groove_source_set(2, GROOVE_303)) return -1;
    for (unsigned t = 0; t < 3; ++t) {
        groove_edit_clear(t, 0, 2);
        groove_sequence_length_set(t, 16);
    }
    /* 909 voicing on T2: the same engine, set tighter and brighter. */
    dp_tone_set(1, D8_BD, "Decay", 46); dp_tone_set(1, D8_BD, "Tune", 84);
    dp_tone_set(1, D8_BD, "Attack", 96); dp_tone_set(1, D8_BD, "Tone", 74);
    dp_tone_set(1, D8_BD, "Drive", 28);
    dp_tone_set(1, D8_SD, "Tone", 96); dp_tone_set(1, D8_SD, "Snappy", 112);
    dp_tone_set(1, D8_SD, "Tune", 82); dp_tone_set(1, D8_SD, "Decay", 74);
    dp_tone_set(1, D8_CP, "Decay", 52); dp_tone_set(1, D8_CP, "Tune", 74);
    dp_tone_set(1, D8_CH, "Decay", 30); dp_tone_set(1, D8_CH, "Tune", 92);
    dp_tone_set(1, D8_OH, "Decay", 58); dp_tone_set(1, D8_OH, "Tune", 92);
    dp_tone_set(1, D8_CY, "Tune", 84);
    dp_tone_set(1, D8_RS, "Decay", 30); dp_tone_set(1, D8_RS, "Tune", 80);
    /* Starter groove: 808 kick and hats, 909 clap and open hats, acid bass. */
    dp_drum_line(0, D8_BD, "X...x...X...x...", 100);
    dp_drum_line(0, D8_CH, "..x...x...x...x.", 90);
    dp_drum_line(0, D8_RS, ".......x......x.", 80);
    dp_drum_line(1, D8_CP, "....X.......X...", 100);
    dp_drum_line(1, D8_OH, "..x...x...x...x.", 90);
    dp_drum_line(1, D8_CH, "x.......x.......", 70);
    static const int8_t acid[16] = {36, -1, 36, 48, -1, 39, 36, -1, 43, -1, 36, 46, -1, 39, 41, -1};
    static const uint8_t accent[16] = {1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0};
    for (unsigned s = 0; s < 16; ++s)
        if (acid[s] >= 0) groove_sequence_edit(2, s, (unsigned)acid[s], accent[s] ? 127U : 96U);
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
    if (dp_edit_down) return 1;
    return dp_note_at && frame_time - dp_note_at < 1200U;
}
static void dp_center(unsigned y, const char *s, unsigned scale, uint16_t fg, uint16_t bg)
{
    unsigned w = (unsigned)strlen(s) * 6U * scale;
    text(w < 200 ? 20 + (200 - w) / 2 : 20, y, s, scale, fg, bg);
}
static unsigned dp_cell_x(unsigned i) { return 36 + i * 12 + (i / 4) * 2; }
static void dp_draw_box(void)
{
    if (paint_rendering && (paint_top >= 176 || paint_top + PAINT_ROWS <= 48)) return;
    unsigned active = dp_box_active();
    uint32_t sig = active | (dp_edit_down << 1);
    if (dp_note_mid) sig = signature(sig, dp_note_mid, (unsigned)strlen(dp_note_mid));
    if (sig == cache->hlv[DP_C_BOX]) return;
    cache->hlv[DP_C_BOX] = sig;
    if (!active) return; /* Rows repaint themselves when the box closes. */
    paint_fill(20, 64, 200, 96, TEXT);
    paint_fill(22, 66, 196, 92, BACK);
    paint_fill(24, 68, 192, 88, TEXT);
    if (dp_edit_down) {
        dp_center(78, "EDIT HELD", 1, BACK, TEXT);
        dp_center(96, "SAVE", 2, BACK, TEXT);
        dp_center(116, "= LOAD 6210 KIT", 1, BACK, TEXT);
        dp_center(136, "PRESETS = ENGINE", 1, BACK, TEXT);
    } else {
        dp_center(80, dp_note_top ? dp_note_top : "", 1, BACK, TEXT);
        dp_center(100, dp_note_mid ? dp_note_mid : "", 3, BACK, TEXT);
    }
}
static void dp_draw_drums(const struct groove_view *sel, unsigned first, unsigned box, unsigned phase)
{
    unsigned pads = dp_pads(), rh = pads > 8 ? 11U : 15U;
    unsigned length = sel->sequence_length ? sel->sequence_length : 16U;
    unsigned head = sel->playing && (sel->play_step & ~15U) == first ? (sel->play_step & 15U) : 255U;
    unsigned flash = phase < 110U;
    for (unsigned r = 0; r < pads && r < 11; ++r) {
        unsigned y = 48 + r * rh;
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + rh <= paint_top)) continue;
        uint64_t lane = groove_drum_steps(track, r);
        uint32_t bits = (uint32_t)((lane >> first) & 0xffffU);
        unsigned hit = head != 255U && ((bits >> head) & 1U) && flash && !sel->muted;
        uint32_t meta = (head & 31U) | ((r == drum) << 5) | (hit << 6) | (box << 7) | ((first / 16U) << 8) |
            ((length / 16U) << 11) | ((cursor - first < 16U ? cursor - first : 31U) << 14) | (pads << 19);
        uint32_t sig = signature(meta, &bits, sizeof bits);
        if (sig == cache->hlv[DP_C_ROW + r]) continue;
        cache->hlv[DP_C_ROW + r] = sig;
        paint_fill(0, y, 240, rh, BACK);
        /* Label: inverted for the selected drum, a short flash on every hit. */
        uint16_t lb = r == drum || hit ? TEXT : BACK, lf = lb == TEXT ? BACK : TEXT;
        if (r == drum && hit) { lb = DIM; lf = BACK; }
        paint_fill(2, y, 30, rh - 2, lb);
        text(4, y + (rh - 9) / 2, groove_pad_label(track, r), 1, lf, lb);
        for (unsigned i = 0; i < 16; ++i) {
            unsigned x = dp_cell_x(i), s = first + i;
            if (s >= length) { frame(x, y, 10, rh - 2, PANEL); continue; }
            unsigned on = (bits >> i) & 1U;
            if (i == head) {
                paint_fill(x, y, 10, rh - 2, TEXT);
                paint_fill(x + 2, y + 2, 6, rh - 6, on ? DIM : BACK);
            } else paint_fill(x, y, 10, rh - 2, on ? TEXT : PANEL);
            if (r == drum && s == cursor && !on && i != head) paint_fill(x + 3, y + (rh - 2) / 2 - 1, 4, 2, TEXT);
        }
    }
}
static void dp_draw_notes(const struct groove_view *sel, unsigned first, unsigned box)
{
    if (paint_rendering && (paint_top >= 172 || paint_top + PAINT_ROWS <= 48)) return;
    struct groove_step cells[16];
    groove_sequence_read(track, first, cells, 16);
    unsigned length = sel->sequence_length ? sel->sequence_length : 16U;
    unsigned head = sel->playing && (sel->play_step & ~15U) == first ? (sel->play_step & 15U) : 255U;
    unsigned low = 127, high = 0;
    for (unsigned i = 0; i < 16; ++i)
        if (cells[i].velocity) {
            if (cells[i].note < low) low = cells[i].note;
            if (cells[i].note > high) high = cells[i].note;
        }
    if (low > high) low = high = 60;
    if (high - low < 12) { low = low > 6 ? low - 6 : 0; high = low + 12; }
    uint32_t sig = signature((head & 31U) | (box << 5) | ((first / 16U) << 6) | ((length / 16U) << 9) |
        ((cursor - first < 16U ? cursor - first : 31U) << 12), cells, sizeof cells);
    if (sig == cache->hlv[DP_C_ROW]) return;
    cache->hlv[DP_C_ROW] = sig;
    paint_fill(0, 46, 240, 126, BACK);
    for (unsigned i = 0; i < 16; ++i) {
        unsigned x = dp_cell_x(i), s = first + i;
        if (s >= length) { frame(x, 160, 10, 10, PANEL); continue; }
        unsigned on = cells[i].velocity != 0;
        unsigned h = on ? 14U + (cells[i].note - low) * 96U / (high - low) : 4U;
        if (h > 110) h = 110;
        paint_fill(x, 170 - h, 10, h, on ? (cells[i].velocity > 100 ? TEXT : DIM) : PANEL);
        if (i == head) { paint_fill(x, 48, 10, 4, TEXT); frame(x, 170 - h, 10, h, TEXT); }
        if (s == cursor) paint_fill(x, 56, 10, 2, DIM);
    }
    text(4, 50, "HI", 1, DIM, BACK);
    text(4, 160, "LO", 1, DIM, BACK);
}
static void dp_draw(const struct groove_view v[8])
{
    const struct groove_view *sel = &v[track];
    unsigned pads = dp_pads(), box = dp_box_active();
    unsigned phase = dp_phase(sel), first = step_first(sel);
    unsigned beat = sel->playing && (sel->play_step & 3U) == 0 && phase < 160U;
    /* Header: track, engine chip, kit number, tempo, beat square. */
    if (!(paint_rendering && paint_top >= 30)) {
        uint32_t sig = track | (sel->type << 3) | ((uint32_t)sel->program << 6) | ((uint32_t)sel->tempo << 14) |
            ((uint32_t)beat << 24) | ((uint32_t)groove_project_dirty() << 25) | ((uint32_t)sel->clock_mode << 26) |
            ((uint32_t)sel->muted << 27);
        if (sig != cache->hlv[DP_C_HEAD]) {
            cache->hlv[DP_C_HEAD] = sig;
            paint_fill(0, 0, 240, 27, BACK);
            text(6, 8, "T", 2, TEXT, BACK);
            number(18, 8, (int)track + 1, 1, 2, TEXT, BACK);
            paint_fill(34, 6, 38, 18, sel->muted ? DIM : TEXT);
            text(36, 8, dp_source_short(sel->type), 2, BACK, sel->muted ? DIM : TEXT);
            text(78, 7, "KIT", 1, DIM, BACK);
            number(78, 16, sel->program + 1, 3, 1, TEXT, BACK);
            if (sel->clock_mode && !sel->tempo) text(108, 8, "EXT", 2, TEXT, BACK);
            else number(108, 8, sel->tempo, 3, 2, TEXT, BACK);
            text(146, 15, "BPM", 1, DIM, BACK);
            text(170, 15, groove_project_dirty() ? "*" : " ", 1, TEXT, BACK);
            text(180, 8, "6210", 1, DIM, BACK);
            frame(220, 6, 16, 16, TEXT);
            paint_fill(223, 9, 10, 10, beat ? TEXT : BACK);
        }
    }
    /* Title: what the grid shows and which 16 steps of how many. */
    if (!(paint_rendering && (paint_top >= 46 || paint_top + PAINT_ROWS <= 30))) {
        unsigned length = sel->sequence_length ? sel->sequence_length : 16U;
        unsigned playing_page = sel->playing ? sel->play_step / 16U : 7U;
        uint32_t sig = drum | (track << 4) | ((first / 16U) << 7) | ((length / 16U) << 10) |
            (playing_page << 13) | ((uint32_t)sel->type << 17) | (step_follow << 20);
        if (sig != cache->hlv[DP_C_TITLE]) {
            cache->hlv[DP_C_TITLE] = sig;
            paint_fill(0, 30, 240, 16, BACK);
            const char *name = pads ? groove_pad_name(track, drum)
                : sel->type == GROOVE_303 ? "ACID 303" : sel->type == GROOVE_DX7 ? "DX7 STEPS"
                : sel->type == GROOVE_PSG ? "PSG STEPS" : "PLUCK STEPS";
            text(4, 34, name, 1, TEXT, BACK);
            for (unsigned p = 0; p < 4; ++p) {
                unsigned x = 156 + p * 20;
                if (p * 16 >= length) { frame(x, 33, 16, 9, PANEL); continue; }
                paint_fill(x, 33, 16, 9, p == first / 16 ? TEXT : PANEL);
                if (p == playing_page) paint_fill(x + 6, 36, 4, 3, p == first / 16 ? BACK : TEXT);
            }
        }
    }
    if (pads) dp_draw_drums(sel, first, box, phase);
    else dp_draw_notes(sel, first, box);
    /* Cursor step details. */
    if (!(paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 174))) {
        unsigned velocity = groove_step_setting(track, cursor, 0), chance = groove_step_setting(track, cursor, 2);
        struct groove_view cell = {0};
        groove_get_view(track, cursor, &cell);
        unsigned on = pads ? (cell.step.drums >> drum) & 1U : cell.step.velocity != 0;
        uint32_t sig = cursor | (velocity << 6) | (chance << 14) | (on << 18) | ((uint32_t)cell.step.note << 19) | (box << 27);
        if (sig != cache->hlv[DP_C_INFO]) {
            cache->hlv[DP_C_INFO] = sig;
            paint_fill(0, 174, 240, 18, BACK);
            text(4, 178, "STEP", 1, DIM, BACK);
            number(32, 178, (int)cursor + 1, 2, 1, TEXT, BACK);
            char value[12];
            if (!pads && on) { note_name(cell.step.note, value); text(52, 178, value, 1, TEXT, BACK); }
            text(84, 178, on ? "ON " : "OFF", 1, on ? TEXT : DIM, BACK);
            text(108, 178, "VEL", 1, DIM, BACK);
            number(128, 178, (int)velocity, 3, 1, TEXT, BACK);
            text(156, 178, velocity >= 120 ? "ACC" : "   ", 1, TEXT, BACK);
            text(180, 178, "CH", 1, DIM, BACK);
            decimal(value, chance ? (int)((chance * 100U + 4U) / 8U) : 100, 1);
            strcat(value, "%");
            text(196, 178, value, 1, TEXT, BACK);
        }
    }
    dp_draw_box();
    /* K1..K4. */
    char value[12];
    if (pads) {
        knob(0, "K1 DRUM", groove_pad_label(track, drum), TEXT);
    } else {
        struct groove_view cell = {0};
        groove_get_view(track, cursor, &cell);
        note_name(cell.step.velocity ? cell.step.note : selected_note, value);
        knob(0, "K1 NOTE", value, TEXT);
    }
    decimal(value, (int)groove_step_setting(track, cursor, 0), 1);
    knob(1, "K2 VELO", value, TEXT);
    unsigned chance = groove_step_setting(track, cursor, 2);
    decimal(value, chance ? (int)((chance * 100U + 4U) / 8U) : 100, 1);
    strcat(value, "%");
    knob(2, "K3 CHANCE", value, TEXT);
    if (pads) {
        uint8_t values[GROOVE_TONE_PARAMS];
        unsigned p = dp_tune_parameter(track, drum);
        if (groove_tone_copy(track, drum, values) < 0) strcpy(value, "--");
        else decimal(value, values[p], 1);
        knob(3, "K4 TUNE", value, TEXT);
    } else if (sel->type == GROOVE_DX7) {
        decimal(value, sel->volume, 1);
        knob(3, "K4 VOLUME", value, TEXT);
    } else {
        uint8_t values[GROOVE_TONE_PARAMS];
        if (groove_tone_copy(track, 0, values) < 0) strcpy(value, "--");
        else decimal(value, values[0], 1);
        knob(3, "K4 CUTOFF", value, TEXT);
    }
    if (!(paint_rendering && paint_top + PAINT_ROWS <= 229)) {
        const char *hint = notice ? notice : dp_edit_down ? "EDIT HELD"
            : pads ? "WHITE=STEPS  BLACK=DRUMS  OCT=PAGE" : "WHITE=STEPS  BLACK=NOTES  OCT=PAGE";
        uint32_t sig = signature(0, hint, (unsigned)strlen(hint));
        if (sig != cache->hlv[DP_C_HINT]) {
            cache->hlv[DP_C_HINT] = sig;
            paint_fill(0, 229, 240, 11, BACK);
            text(4, 231, hint, 1, DIM, BACK);
        }
    }
}
