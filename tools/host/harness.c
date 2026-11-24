/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * 6210 host test harness.
 *
 * Links the real app and screen code with the browser simulator platform
 * (web/engine/Simulator.c) as a native program, then plays a script of panel
 * gestures read from stdin and saves LCD screenshots and LED states.
 *
 * Script commands (one per line, '#' starts a comment):
 *   boot                 clear storage and start the instrument
 *   startup MS           clear storage, start with the boot animation, run MS
 *   wait MS              run the engine for MS milliseconds
 *   button N 1|0         press or release panel button N (0..13)
 *   tap N                press button N, wait 40 ms, release, wait 40 ms
 *   key N 1|0            press or release keyboard key N (0..26)
 *   hit N                press key N, wait 40 ms, release, wait 40 ms
 *   turn N DELTA         turn encoder N (0..6) by DELTA detents
 *   source T S           set track T to sound source S (0 DX7 .. 5 sampler)
 *   shot NAME            write NAME.rgb565 (240x240) and NAME.leds (41 chars)
 *   expect-led N 0|1     fail unless LED N is in that state
 *   tracks               print type, mute, solo and step count of each track
 *   pixels MAX           print LCD pixels sent since the last count; fail above MAX
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void sim_boot(unsigned clear);
void sim_control(unsigned kind, unsigned index, int value);
const float *sim_render(void);
const uint16_t *sim_pixels(void);
const uint8_t *sim_state(void);
int sim_select(unsigned track, unsigned source);
unsigned groove_track_role(unsigned track);

static unsigned elapsed;
extern unsigned long sim_pixels_sent;
static void run(unsigned ms)
{
    /* One render block is 128 frames at 44.1 kHz, about 2.9 ms. */
    unsigned blocks = (ms * 441U + 1279U) / 1280U;
    for (unsigned i = 0; i < blocks; ++i) sim_render();
    elapsed += ms;
}
static void save(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s.rgb565", name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(sim_pixels(), 2, 240 * 240, f);
    fclose(f);
    snprintf(path, sizeof path, "%s.leds", name);
    f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    const uint8_t *state = sim_state();
    for (unsigned i = 0; i < 41; ++i) fputc(state[168 + i] ? '1' : '0', f);
    fputc('\n', f);
    fclose(f);
}
int main(void)
{
    char line[256];
    unsigned number = 0, failures = 0;
    while (fgets(line, sizeof line, stdin)) {
        ++number;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char word[32] = {0}, name[200] = {0};
        int a = 0, b = 0;
        int n = sscanf(line, "%31s", word);
        if (n < 1) continue;
        if (!strcmp(word, "boot")) { sim_boot(1); run(1500); }
        else if (!strcmp(word, "startup") && sscanf(line, "%*s %d", &a) == 1) { sim_boot(3); run((unsigned)a); }
        else if (!strcmp(word, "wait") && sscanf(line, "%*s %d", &a) == 1) run((unsigned)a);
        else if (!strcmp(word, "button") && sscanf(line, "%*s %d %d", &a, &b) == 2) { sim_control(0, (unsigned)a, b); run(20); }
        else if (!strcmp(word, "tap") && sscanf(line, "%*s %d", &a) == 1) {
            sim_control(0, (unsigned)a, 1); run(40); sim_control(0, (unsigned)a, 0); run(40);
        } else if (!strcmp(word, "key") && sscanf(line, "%*s %d %d", &a, &b) == 2) { sim_control(1, (unsigned)a, b); run(20); }
        else if (!strcmp(word, "hit") && sscanf(line, "%*s %d", &a) == 1) {
            sim_control(1, (unsigned)a, 1); run(40); sim_control(1, (unsigned)a, 0); run(40);
        } else if (!strcmp(word, "turn") && sscanf(line, "%*s %d %d", &a, &b) == 2) {
            sim_control(2, (unsigned)a, b); run(15); sim_control(2, (unsigned)a, 0); run(40);
        } else if (!strcmp(word, "source") && sscanf(line, "%*s %d %d", &a, &b) == 2) {
            if (sim_select((unsigned)a, (unsigned)b)) { fprintf(stderr, "line %u: source failed\n", number); ++failures; }
            run(100);
        } else if (!strcmp(word, "shot") && sscanf(line, "%*s %199s", name) == 1) save(name);
        else if (!strcmp(word, "pixels") && sscanf(line, "%*s %d", &a) == 1) {
            printf("pixels: %lu sent\n", sim_pixels_sent);
            if (sim_pixels_sent > (unsigned long)a) { printf("line %u: more than %d pixels\n", number, a); ++failures; }
            sim_pixels_sent = 0;
        } else if (!strcmp(word, "expect-led") && sscanf(line, "%*s %d %d", &a, &b) == 2) {
            const uint8_t *state = sim_state();
            if ((state[168 + a] != 0) != (b != 0)) {
                fprintf(stderr, "line %u: LED %d is %d, expected %d\n", number, a, state[168 + a] != 0, b);
                ++failures;
            }
        } else if (!strcmp(word, "tracks")) {
            const uint8_t *state = sim_state();
            for (unsigned t = 0; t < 8; ++t) {
                const uint8_t *v = state + 8 + t * 20;
                uint64_t steps; memcpy(&steps, v + 12, 8);
                printf("T%u type %u role %u mute %u solo %u playing %u steps %d\n", t + 1, v[0], groove_track_role(t), v[4], v[5], v[6],
                       __builtin_popcountll(steps));
            }
        } else { fprintf(stderr, "line %u: cannot read '%s'\n", number, word); return 2; }
    }
    if (failures) { fprintf(stderr, "%u check(s) failed\n", failures); return 1; }
    printf("host test passed (%u ms simulated)\n", elapsed);
    return 0;
}
