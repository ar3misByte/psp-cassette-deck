/* preview2.c - renders the deck in every theme, the cassette-only view and the menu */
#include "gfx.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
static uint32_t fb[BUF_W * SCR_H];
static void save(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n480 272\n255\n");
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
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
    UiState s;
    memset(&s, 0, sizeof s);
    s.battery = 72; s.hour = 19; s.minute = 50;
    s.tape_name = "Road Trip Mix"; s.tape_style = 0; s.side = 'A'; s.track_no = 3; s.tracks_on_side = 7;
    s.title = "Hotel California"; s.artist = "Eagles"; s.pos = 95; s.dur = 391; s.side_progress = 0.35f;
    s.mode = 1; s.angle_l = 0.4f; s.angle_r = 1.1f; s.counter = 127.4f; s.vu_l = 0.72f; s.vu_r = 0.84f;
    s.peak_r = 1; s.volume = 14; s.sleep_min = -1; s.auto_reverse = 1; s.insert = 1; s.boost_db = 6; s.remote = 1;
    for (int t = 0; t < NUM_THEMES; t++) {
        ui_set_theme(t);
        s.tape_style = t;
        ui_draw_deck(&s);
        char n[32];
        sprintf(n, "theme%d.ppm", t);
        save(n);
    }
    ui_set_theme(3);
    s.tape_style = 3;
    ui_draw_cassette_view(&s);
    save("cassette_view.ppm");
    ui_set_theme(0);
    s.tape_style = 0;
    static const char *labels[6] = {"Theme", "Volume boost", "Auto reverse", "Sleep timer", "View", "Close"};
    static const char *vals[6] = {"Black Deck", "+6 dB", "On", "Off", "Cassette only", ""};
    s.menu_open = 1; s.menu_sel = 1; s.menu_n = 6;
    for (int i = 0; i < 6; i++) { s.menu_label[i] = labels[i]; s.menu_value[i] = vals[i]; }
    ui_draw_deck(&s);
    ui_draw_menu(&s);
    save("menu.ppm");
    return 0;
}
