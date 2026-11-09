/* preview.c - renders the UI on a PC to PPM images, to check the look without a PSP.
 * Build: gcc -O2 -I../src preview.c ../src/gfx.c ../src/ui.c -lm -o preview
 */
#include "gfx.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

static uint32_t fb[BUF_W * SCR_H];

static void save(const char *path, int x0, int y0, int w, int h)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            uint32_t c = fb[y * BUF_W + x];
            unsigned char p[3] = {C_R(c), C_G(c), C_B(c)};
            fwrite(p, 1, 3, f);
        }
    fclose(f);
}

int main(void)
{
    gfx_set_target(fb);
    ui_init();
    UiTape tapes[] = {
        {"Road Trip Mix", 14, 0, 3120}, {"Lo-fi Study Beats", 22, 1, 4300}, {"Arijit Singh Hits", 12, 2, 2900},
        {"Synthwave Nights", 10, 3, 2500}, {"Rock Classics", 16, 4, 3800}, {"Summer Pop", 9, 5, 1900},
        {"A.R. Rahman", 18, 2, 4200}, {"Workout", 11, 4, 2400}, {"Jazz Sunday", 8, 1, 2800}, {"Extra", 5, 0, 900},
    };
    UiState s;
    memset(&s, 0, sizeof s);
    s.battery = 72; s.hour = 17; s.minute = 42;
    s.tapes = tapes; s.ntapes = 10; s.sel = 2; s.scroll = 0; s.sel_slide = 1;
    s.resume_hint = "Resume: Road Trip Mix, side B track 3";
    ui_draw_rack(&s);
    save("rack.ppm", 0, 0, SCR_W, SCR_H);

    memset(&s, 0, sizeof s);
    s.battery = 72; s.hour = 17; s.minute = 42;
    s.tape_name = "Road Trip Mix"; s.tape_style = 0; s.side = 'A'; s.track_no = 3; s.tracks_on_side = 7;
    s.title = "Hotel California"; s.artist = "Eagles"; s.pos = 95; s.dur = 391; s.side_progress = 0.35f;
    s.mode = MODE_PLAY; s.angle_l = 0.4f; s.angle_r = 1.1f; s.counter = 127.4f; s.vu_l = 0.72f; s.vu_r = 0.84f;
    s.peak_r = 1; s.volume = 14; s.sleep_min = 30; s.auto_reverse = 1; s.insert = 1;
    ui_draw_deck(&s);
    save("deck.ppm", 0, 0, SCR_W, SCR_H);

    s.tape_style = 3; s.side = 'B'; s.side_progress = 0.85f; s.mode = MODE_FF; s.toast = "Side B"; s.toast_alpha = 1;
    ui_draw_deck(&s);
    save("deck2.ppm", 0, 0, SCR_W, SCR_H);

    s.flip = 0.3f; s.toast = 0;
    ui_draw_deck(&s);
    save("deck_flip.ppm", 0, 0, SCR_W, SCR_H);

    /* ICON0: 144x80 */
    gfx_clear(RGB(0, 0, 0));
    gfx_vgrad(0, 0, 144, 80, RGB(40, 40, 46), RGB(16, 16, 18));
    ui_draw_cassette(4, 4, 1.36f, 1.15f, 0, "", 'A', 0.4f, 0.2f, 0.9f, 1);
    save("icon0.ppm", 0, 0, 144, 80);

    /* PIC1: 480x272 background shown in the XMB */
    s.toast = 0; s.flip = 0; s.mode = MODE_PLAY; s.side = 'A'; s.tape_style = 0;
    gfx_vgrad(0, 0, SCR_W, SCR_H, RGB(36, 30, 26), RGB(10, 8, 8));
    ui_draw_cassette(150, 60, 3.0f, 3.0f, 2, "Cassette Deck", 'A', 0.3f, 0.5f, 1.2f, 1);
    gfx_rect_alpha(0, 0, SCR_W, SCR_H, RGB(0, 0, 0), 60);
    save("pic1.ppm", 0, 0, SCR_W, SCR_H);
    return 0;
}
