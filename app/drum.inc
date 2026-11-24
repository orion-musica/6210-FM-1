/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * 6210 Drum Peter - DRUM page, kits, songs and the project menu for FM1 Open
 * (two-tone LCD edition).
 *
 * Included once by screen.c, after its drawing helpers and before
 * apply_controls(). Everything here runs on the control task and uses only
 * the public instrument API (instrument.h). The engine's own sequencer plays
 * the notes, so timing stays sample accurate.
 *
 * Four drum tracks, all on the DRUM 808 engine: T1 808, T2 909, T3 606 and
 * T4 707 (the last three are voicings). Tracks 5..8 are switched off.
 *
 * DRUM page:
 *   16 white keys  steps of the selected drum (one page of 16)
 *   black keys     pick a drum and hear it (11 drums)
 *   OCT - / +      previous / next 16 steps
 *   SELECT         track 1..4        ALGORITHM  length 16/32/48/64
 *   PRESETS        kit program       K1 drum, K2 velocity, K3 chance, K4 tune
 * SAVE opens the project menu: SAVE PROJECT, LOAD SAVED (the 16 user slots;
 * EDIT deletes one after YES / NO), LOAD TEMPLATE (the eight built-in songs)
 * and NEW PROJECT (asks YES / NO first).
 */
#include "drum808.h"

enum { DP_C_HEAD, DP_C_TITLE, DP_C_ROW, DP_C_BOX = DP_C_ROW + 11, DP_C_HINT, DP_C_REST, DP_C_COUNT };
_Static_assert(DP_C_COUNT <= 16, "DRUM page cache slots");

enum { DP_TRACKS = 4, DP_SONGS = 8 };
static uint8_t dp_audition[GROOVE_PADS];
static unsigned dp_setup_checked;
static uint32_t dp_step_at, dp_step_ms = 115;
static unsigned dp_step_seen = 255;

static void dp_init(void)
{
    memset(dp_audition, 0xff, sizeof dp_audition);
    dp_setup_checked = dp_item = dp_confirm = 0;
}
static unsigned dp_pads(void) { return groove_pad_count(track); }
static const char *dp_source_short(unsigned type)
{
    static const char *const names[] = {"DX7", "303", "DRM", "PSG", "PLK", "SMP"};
    return type < GROOVE_SOURCES ? names[type] : "---";
}
static unsigned dp_black(unsigned key)
{
    /* Black keys in panel order; returns 0..10 or 255 for a white key. */
    for (unsigned i = 0; i < 10; ++i) if (edit_keys[i] == key) return i;
    return key == 25 ? 10U : 255U;
}
static const char *dp_kit_name(unsigned t)
{
    static const char *const names[DP_TRACKS] = {"808", "909", "606", "707"};
    return t < DP_TRACKS ? names[t] : "OFF";
}

/* ---- Kits ------------------------------------------------------------------ */
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
        if (name) {
            const char *a = d->name, *b = name;
            while (*a && *b && (*a | 32) == (*b | 32)) { ++a; ++b; }
            if (*a || *b) continue;
        } else value = d->def; /* No name: every parameter back to its default. */
        if (value > d->max) value = d->max;
        if (value != values[p]) groove_tone_edit(t, part, p, (int)value - (int)values[p]);
        if (name) return;
    }
}
/* 909, 606 and 707 voicings of the 808 engine: kit (1..3), part, name, value. */
static const struct { uint8_t kit, part; char name[7]; uint8_t value; } dp_voicing[] = {
    /* 909: short punchy kick, bright snappy snare, crisp hats. */
    {1, D8_BD, "Decay", 46}, {1, D8_BD, "Tune", 84}, {1, D8_BD, "Attack", 96}, {1, D8_BD, "Tone", 74}, {1, D8_BD, "Drive", 28},
    {1, D8_SD, "Tone", 96}, {1, D8_SD, "Snappy", 112}, {1, D8_SD, "Tune", 82}, {1, D8_SD, "Decay", 74},
    {1, D8_CP, "Decay", 52}, {1, D8_CP, "Tune", 74}, {1, D8_CH, "Decay", 30}, {1, D8_CH, "Tune", 92},
    {1, D8_OH, "Decay", 58}, {1, D8_OH, "Tune", 92}, {1, D8_CY, "Tune", 84}, {1, D8_RS, "Decay", 30}, {1, D8_RS, "Tune", 80},
    /* 606: small, high and dry. */
    {2, D8_BD, "Decay", 30}, {2, D8_BD, "Tune", 92}, {2, D8_BD, "Tone", 84},
    {2, D8_SD, "Tune", 92}, {2, D8_SD, "Tone", 104}, {2, D8_SD, "Snappy", 100}, {2, D8_SD, "Decay", 50},
    {2, D8_LT, "Tune", 88}, {2, D8_LT, "Decay", 40}, {2, D8_HT, "Tune", 96}, {2, D8_HT, "Decay", 36},
    {2, D8_CH, "Decay", 20}, {2, D8_CH, "Tune", 100}, {2, D8_OH, "Decay", 40}, {2, D8_OH, "Tune", 100},
    {2, D8_CY, "Decay", 50}, {2, D8_CY, "Tune", 96},
    /* 707: round, punchy and clean. */
    {3, D8_BD, "Decay", 55}, {3, D8_BD, "Tune", 70}, {3, D8_BD, "Attack", 110}, {3, D8_BD, "Tone", 60}, {3, D8_BD, "Drive", 10},
    {3, D8_SD, "Tone", 80}, {3, D8_SD, "Snappy", 96}, {3, D8_SD, "Decay", 60}, {3, D8_SD, "Tune", 70},
    {3, D8_CP, "Decay", 60}, {3, D8_RS, "Decay", 35}, {3, D8_RS, "Tune", 75}, {3, D8_CB, "Tune", 70},
    {3, D8_LT, "Tune", 72}, {3, D8_LT, "Decay", 60}, {3, D8_CH, "Decay", 36}, {3, D8_CH, "Tune", 80},
    {3, D8_OH, "Decay", 64}, {3, D8_OH, "Tune", 80}, {3, D8_CY, "Decay", 90}};
static void dp_voice(unsigned t)
{
    for (unsigned part = 0; part <= D8_KIT; ++part) dp_tone_set(t, part, 0, 0);
    for (unsigned i = 0; i < sizeof dp_voicing / sizeof dp_voicing[0]; ++i)
        if (dp_voicing[i].kit == t) dp_tone_set(t, dp_voicing[i].part, dp_voicing[i].name, dp_voicing[i].value);
}

/* ---- Songs ----------------------------------------------------------------- */
static const struct { char name[12]; uint8_t tempo, swing, first, count; } dp_songs[] = {
    {"HYPNO 132", 132, 0, 0, 6},
    {"HARD 145", 145, 0, 6, 6},
    {"ELECTRO 126", 126, 0, 12, 6},
    {"HOUSE 124", 124, 16, 18, 5},
    {"MINIMAL 128", 128, 8, 23, 6},
    {"BREAKS 136", 136, 0, 29, 5},
    {"DUB 118", 118, 10, 34, 5},
    {"BOOM BAP 90", 90, 20, 39, 5},
};
static const struct { uint8_t track, pad; uint16_t steps, accents; } dp_lines[] = {
    {1, D8_BD, 0x1111, 0x0101}, /* HYPNO 13 909 BD X...x...X...x... */
    {1, D8_OH, 0x4444, 0x0000}, /* HYPNO 13 909 OH ..x...x...x...x. */
    {2, D8_CH, 0xffff, 0x4444}, /* HYPNO 13 606 CH xxXxxxXxxxXxxxXx */
    {0, D8_RS, 0x2420, 0x0000}, /* HYPNO 13 808 RS .....x....x..x.. */
    {3, D8_CP, 0x1010, 0x0000}, /* HYPNO 13 707 CP ....x.......x... */
    {0, D8_LT, 0x0080, 0x0000}, /* HYPNO 13 808 LT .......x........ */
    {1, D8_BD, 0x1111, 0x1111}, /* HARD 145 909 BD X...X...X...X... */
    {0, D8_BD, 0xc444, 0x0000}, /* HARD 145 808 BD ..x...x...x...xx */
    {3, D8_CP, 0x1010, 0x1010}, /* HARD 145 707 CP ....X.......X... */
    {1, D8_CH, 0xffff, 0x0000}, /* HARD 145 909 CH xxxxxxxxxxxxxxxx */
    {1, D8_CY, 0x4444, 0x0000}, /* HARD 145 909 CY ..x...x...x...x. */
    {2, D8_OH, 0x4444, 0x0000}, /* HARD 145 606 OH ..x...x...x...x. */
    {0, D8_BD, 0x0481, 0x0001}, /* ELECTRO  808 BD X......x..x..... */
    {0, D8_SD, 0x1010, 0x1010}, /* ELECTRO  808 SD ....X.......X... */
    {0, D8_CH, 0xd555, 0x0000}, /* ELECTRO  808 CH x.x.x.x.x.x.x.xx */
    {0, D8_CB, 0x0208, 0x0000}, /* ELECTRO  808 CB ...x.....x...... */
    {2, D8_HT, 0x4000, 0x0000}, /* ELECTRO  606 HT ..............x. */
    {3, D8_CP, 0x1010, 0x0000}, /* ELECTRO  707 CP ....x.......x... */
    {1, D8_BD, 0x1111, 0x0101}, /* HOUSE 12 909 BD X...x...X...x... */
    {3, D8_CP, 0x1010, 0x1010}, /* HOUSE 12 707 CP ....X.......X... */
    {1, D8_OH, 0x4444, 0x0000}, /* HOUSE 12 909 OH ..x...x...x...x. */
    {2, D8_CH, 0x6d6d, 0x0000}, /* HOUSE 12 606 CH x.xx.xx.x.xx.xx. */
    {3, D8_RS, 0x4080, 0x0000}, /* HOUSE 12 707 RS .......x......x. */
    {0, D8_BD, 0x1111, 0x0101}, /* MINIMAL  808 BD X...x...X...x... */
    {3, D8_RS, 0x2424, 0x0000}, /* MINIMAL  707 RS ..x..x....x..x.. */
    {2, D8_CH, 0xaaaa, 0x0000}, /* MINIMAL  606 CH .x.x.x.x.x.x.x.x */
    {1, D8_CP, 0x0010, 0x0000}, /* MINIMAL  909 CP ....x........... */
    {0, D8_CB, 0x0200, 0x0000}, /* MINIMAL  808 CB .........x...... */
    {2, D8_OH, 0x4000, 0x0000}, /* MINIMAL  606 OH ..............x. */
    {1, D8_BD, 0x0441, 0x0001}, /* BREAKS 1 909 BD X.....x...x..... */
    {1, D8_SD, 0x1290, 0x1010}, /* BREAKS 1 909 SD ....X..x.x..X... */
    {2, D8_CH, 0x5555, 0x0000}, /* BREAKS 1 606 CH x.x.x.x.x.x.x.x. */
    {3, D8_OH, 0x4000, 0x0000}, /* BREAKS 1 707 OH ..............x. */
    {0, D8_RS, 0x2082, 0x0000}, /* BREAKS 1 808 RS .x.....x.....x.. */
    {0, D8_BD, 0x1111, 0x0101}, /* DUB 118  808 BD X...x...X...x... */
    {3, D8_RS, 0x0408, 0x0000}, /* DUB 118  707 RS ...x......x..... */
    {1, D8_OH, 0x4444, 0x0000}, /* DUB 118  909 OH ..x...x...x...x. */
    {2, D8_CH, 0xaaaa, 0x0000}, /* DUB 118  606 CH .x.x.x.x.x.x.x.x */
    {0, D8_CP, 0x1010, 0x0000}, /* DUB 118  808 CP ....x.......x... */
    {0, D8_BD, 0x2301, 0x0001}, /* BOOM BAP 808 BD X.......xx...x.. */
    {3, D8_SD, 0x1010, 0x1010}, /* BOOM BAP 707 SD ....X.......X... */
    {2, D8_CH, 0x5555, 0x0000}, /* BOOM BAP 606 CH x.x.x.x.x.x.x.x. */
    {1, D8_RS, 0x4080, 0x0000}, /* BOOM BAP 909 RS .......x......x. */
    {0, D8_OH, 0x4000, 0x0000}, /* BOOM BAP 808 OH ..............x. */
};

static const char *dp_song_name(unsigned song) { return song < DP_SONGS ? dp_songs[song].name : 0; }
/* An empty project: the four kits with no steps, ARP off, nothing muted,
 * tracks 5..8 off. Saved slots in Flash are not touched. */
static void dp_new(unsigned tempo, unsigned swing, const char *name)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        struct groove_view v = {0};
        groove_get_view(t, 0, &v);
        if (groove_arp_enabled(t)) groove_arp_set(t, 0);
        if (v.muted) groove_mix_toggle(t, 0);
        if (v.solo) groove_mix_toggle(t, 1);
        int role = t < DP_TRACKS ? GROOVE_TRACK_NORMAL : GROOVE_TRACK_OFF;
        if (t < DP_TRACKS || groove_track_role(t) != GROOVE_TRACK_OFF) {
            if (groove_track_role(t) == GROOVE_TRACK_NORMAL) groove_edit_clear(t, 0, 2);
            groove_settings_track(t, 0, role - (int)groove_track_role(t));
        }
        if (t >= DP_TRACKS) continue;
        if (groove_source_type(t) != GROOVE_808) groove_source_set(t, GROOVE_808);
        groove_edit_clear(t, 0, 2);
        groove_sequence_length_set(t, 16);
        dp_voice(t);
    }
    struct groove_view v = {0};
    groove_get_view(0, 0, &v);
    if (!v.clock_mode && v.tempo) groove_tempo_edit((int)tempo - (int)v.tempo);
    groove_sequence_setting_edit(0, (int)swing - (int)groove_sequence_setting(0));
    for (unsigned i = 0; i < 12; ++i) {
        char c = *name ? *name++ : ' ';
        groove_project_rename(i, c);
    }
    track = drum = cursor = 0; step_follow = 1;
    selected_note = groove_pad_note(0, 0);
}
static int dp_song_load(unsigned song)
{
    if (song >= DP_SONGS) return GROOVE_PROJECT_EMPTY;
    groove_edit_checkpoint(0);
    dp_new(dp_songs[song].tempo, dp_songs[song].swing, dp_songs[song].name);
    /* Every hit at velocity 96, then the accents again at 127 (velocity is per step). */
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned i = dp_songs[song].first; i < dp_songs[song].first + dp_songs[song].count; ++i) {
            unsigned bits = pass ? dp_lines[i].accents : dp_lines[i].steps;
            for (unsigned s = 0; s < 16; ++s)
                if ((bits >> s) & 1U)
                    groove_sequence_edit(dp_lines[i].track, s, groove_pad_note(dp_lines[i].track, dp_lines[i].pad), pass ? 127U : 96U);
        }
    return 0;
}
/* A fresh start has an empty project (no notes on any track). The first time
 * 6210 sees one it loads song 1. A project with notes in it is never touched. */
static void dp_setup_once(void)
{
    if (dp_setup_checked || boot_active) return;
    dp_setup_checked = 1;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        struct groove_view v = {0};
        groove_get_view(t, 0, &v);
        if (v.active_steps) return;
    }
    if (!dp_song_load(0)) {
        preset_loaded = 1; preset_current_bank = 1; preset_current_slot = 0;
        notice = "SONG 1 LOADED"; notice_until = fm1_millis() + 2500U;
    }
}

/* ---- Project menu (SAVE) --------------------------------------------------- */
static void dp_menu_open(void)
{
    dp_item = dp_confirm = 0;
    page = FILES;
}
/* HOME (cancel) is handled by the panel buttons. */
static void dp_menu_controls(const struct fm1_controls *c, uint32_t pressed, uint32_t key_down, uint32_t now)
{
    blocked_keys |= key_down; /* A menu key must not also write a step on the next page. */
    int move = c->encoders[0] + c->encoders[2] + c->encoders[6];
    unsigned ok = (pressed & BUTTON(SAVE)) != 0, no = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (key_down & (1U << white_keys[i]) && (!dp_confirm || i < 2)) {
            if (dp_confirm) { ok = i == 0; no = i == 1; }
            else { dp_item = i; ok = 1; }
        }
    if (dp_confirm) {
        if (no || move) { dp_confirm = 0; return; }
        if (!ok) return;
        if (!release_keys()) return;
        groove_edit_checkpoint(0);
        dp_new(130, 0, "NEW PROJECT");
        page = music_page = DRUM;
        notice = "NEW PROJECT"; notice_until = now + 2000U;
        return;
    }
    if (move) dp_item = (unsigned)clamp((int)dp_item + move, 0, 3);
    if (!ok) return;
    if (dp_item == 0) {
        project_return = DRUM; page = PROJECT; project_result = 0;
        project_reload = project_operation = 0;
        project_slot = groove_project_slot(); project_factory = name_cursor = 0;
        project_refresh();
    } else if (dp_item <= 2) {
        /* LOAD SAVED: the user slots. LOAD TEMPLATE: the built-in songs. */
        preset_lock = 1; preset_bank = dp_item - 1;
        preset_slot = preset_bank ? (preset_current_bank ? preset_current_slot : 0) : groove_project_slot();
        project_result = 0; preset_refresh(); page = PRESET_BROWSER;
    } else dp_confirm = 1;
}
static void dp_menu_draw(void)
{
    char name[13];
    groove_project_name(name);
    uint32_t sig = signature(dp_item | (dp_confirm << 2), name, 12);
    if (cache->hlv[0] == sig) return;
    cache->hlv[0] = sig;
    paint_fill(0, 0, 240, 240, BACK);
    text(6, 8, "PROJECT", 2, TEXT, BACK);
    text(6, 32, name, 1, DIM, BACK);
    paint_fill(0, 44, 240, 2, TEXT);
    if (dp_confirm) {
        paint_fill(10, 60, 220, 70, TEXT);
        text(22, 72, "CLEAR THE CURRENT", 1, BACK, TEXT);
        text(22, 86, "PROJECT?", 2, BACK, TEXT);
        text(22, 110, "SAVED SLOTS STAY", 1, BACK, TEXT);
        frame(10, 144, 104, 40, TEXT); frame(126, 144, 104, 40, TEXT);
        paint_fill(13, 147, 98, 34, TEXT);
        text(38, 156, "YES", 2, BACK, TEXT);
        text(164, 156, "NO", 2, TEXT, BACK);
        text(6, 210, "KEY 1 / SAVE = YES", 1, DIM, BACK);
        text(6, 224, "KEY 2 / HOME = NO", 1, DIM, BACK);
        return;
    }
    static const char *const items[4] = {"SAVE PROJECT", "LOAD SAVED", "LOAD TEMPLATE", "NEW PROJECT"};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned y = 54 + i * 38, on = i == dp_item;
        paint_fill(6, y, 228, 32, on ? TEXT : PANEL);
        number(14, y + 9, (int)i + 1, 1, 2, on ? BACK : DIM, on ? TEXT : PANEL);
        text(40, y + 9, items[i], 2, on ? BACK : TEXT, on ? TEXT : PANEL);
    }
    text(6, 210, "SELECT / KEYS 1-4", 1, DIM, BACK);
    text(6, 224, "SAVE = OPEN  HOME = BACK", 1, DIM, BACK);
}

/* ---- Delete a saved project (load list, user slots) ------------------------- */
static int dp_browser_gesture(const struct fm1_controls *c, uint32_t pressed, uint32_t key_down)
{
    if (page != PRESET_BROWSER) { dp_delete = 0; return 0; }
    if (!dp_delete) {
        if (!(pressed & BUTTON(EDIT)) || preset_bank) return 0;
        if (project_stored_result) { project_result = GROOVE_PROJECT_EMPTY; return 1; }
        dp_delete = 1; cache->shown_page = ~0U;
        return 1;
    }
    blocked_keys |= key_down;
    unsigned yes = (pressed & BUTTON(SAVE)) || (key_down & (1U << white_keys[0]));
    unsigned no = (pressed & ~BUTTON(SAVE)) || (key_down & ~(1U << white_keys[0])) || encoder_active(c->encoders);
    if (!yes && !no) return 1;
    dp_delete = 0; cache->shown_page = ~0U;
    if (yes) {
        project_result = groove_project_delete_slot(preset_slot);
        if (project_result == GROOVE_PROJECT_EMPTY) project_result = 0;
        if (!project_result && !preset_current_bank && preset_current_slot == preset_slot) preset_loaded = 0;
        unsigned keep = project_result; preset_refresh(); project_result = (int)keep;
    }
    return 1;
}
static void dp_delete_draw(void)
{
    uint32_t sig = signature(preset_slot, preset_names[preset_slot], 12);
    if (cache->hlv[1] == sig) return;
    cache->hlv[1] = sig;
    paint_fill(0, 30, 240, 210, BACK);
    text(8, 38, "DELETE A SAVED PROJECT", 1, DIM, BACK);
    paint_fill(10, 52, 220, 84, TEXT);
    number(22, 62, (int)preset_slot + 1, 2, 2, BACK, TEXT);
    text(52, 62, preset_names[preset_slot], 2, BACK, TEXT);
    text(22, 88, "DELETE IT?", 2, BACK, TEXT);
    text(22, 114, "THIS CANNOT BE UNDONE", 1, BACK, TEXT);
    frame(10, 148, 104, 40, TEXT); frame(126, 148, 104, 40, TEXT);
    paint_fill(13, 151, 98, 34, TEXT);
    text(38, 160, "YES", 2, BACK, TEXT);
    text(164, 160, "NO", 2, TEXT, BACK);
    text(6, 210, "KEY 1 / SAVE = YES", 1, DIM, BACK);
    text(6, 224, "KEY 2 / ANY OTHER = NO", 1, DIM, BACK);
}

/* ---- Controls -------------------------------------------------------------- */
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
    unsigned pads = dp_pads(), box = 0;
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
            text(36, 8, pads ? dp_kit_name(track) : dp_source_short(sel->type), 2, BACK, sel->muted ? DIM : TEXT);
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
            text(4, 34, pads ? groove_pad_name(track, drum) : "STEPS", 1, TEXT, BACK);
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
        const char *hint = notice ? notice : "WHITE STEPS  BLACK DRUMS  SAVE MENU";
        uint32_t sig = signature(0, hint, (unsigned)strlen(hint));
        if (sig != cache->hlv[DP_C_HINT]) {
            cache->hlv[DP_C_HINT] = sig;
            paint_fill(0, 229, 240, 11, BACK);
            text(4, 231, hint, 1, DIM, BACK);
        }
    }
}
