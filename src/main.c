/*
 * Cassette Deck - a retro tape-deck music player for the PSP
 *
 * Each folder of MP3s under ms0:/MUSIC becomes a cassette. Its tracks are
 * split into Side A and Side B. Spinning reels, a rolling tape counter and
 * analog VU meters follow the music.
 */
#include <pspkernel.h>
#include <psputils.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspiofilemgr.h>
#include <psphprm.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gfx.h"
#include "ui.h"
#include "player.h"
#include "mp3info.h"

PSP_MODULE_INFO("Cassette Deck", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

#define MAX_TAPES 200
#define MAX_TRACKS 200
#define MAX_SUBDIRS 64
#define TWO_PI 6.2831853f

/* ------------------------------------------------------------------ */
/* data                                                                */

typedef struct {
    char name[64];
    char dir[384];
    int ntracks;
    int style;
    float total;
} Tape;

typedef struct {
    char path[512];
    char title[96];
    char artist[64];
    float dur;
} Track;

static Tape tapes[MAX_TAPES];
static UiTape uitapes[MAX_TAPES];
static int ntapes = 0;

static Track tracks[MAX_TRACKS];
static int ntracks = 0;
static int cur_tape = -1;

static char app_dir[256] = "ms0:/PSP/GAME/CASSETTE";

/* ------------------------------------------------------------------ */
/* state                                                               */

enum { SCR_RACK, SCR_DECK };

static volatile int running = 1;
static int screen = SCR_RACK;

/* rack */
static int rack_sel = 0;
static float rack_scroll = 0, rack_scroll_target = 0, rack_slide = 0;

/* deck */
static int side = 0;   /* 0 = A, 1 = B */
static int idx = 0;    /* global track index */
static int mode = MODE_STOP;
static float pos = 0;  /* seconds within current track */
static int sync_gen = -1;
static int engine_loaded = 0;
static int error_run = 0;

static int cue_dir = 0, cue_prev_mode = MODE_STOP;
static float cue_pos = 0;

static float flip_t = 0;     /* 0 = idle, else 0..1 */
static float insert_t = 1;   /* 1 = inserted */
static int inserting = 0, ejecting = 0, play_after_insert = 0;

static float angle_l = 0, angle_r = 0, counter_val = 0, tape_speed = 0;
static float vu_l = 0, vu_r = 0, vu_vl = 0, vu_vr = 0, peak_l_t = 0, peak_r_t = 0;

static int vol_step = 14;
static const int sleep_opts[] = {-1, 15, 30, 60, 90};
static int sleep_idx = 0;
static float sleep_left = -1;
static int auto_rev = 1;
static int screen_off = 0; /* view == VIEW_OFF */
enum { VIEW_FULL = 0, VIEW_CASSETTE, VIEW_OFF };
static int view = VIEW_FULL;
static int theme = 0;
static int boost_i = 0;
static const int boost_db[5] = {0, 3, 6, 9, 12};
static const float boost_gain[5] = {1.0f, 1.413f, 1.995f, 2.818f, 3.981f};
static int menu_open = 0, menu_sel = 0;
static int cue_src = 0; /* 0 = shoulder buttons, 1 = earphone remote */
static unsigned rm_now = 0, rm_old = 0;
static float rm_fwd_t = 0, rm_back_t = 0;
static int remote_present = 0;
static float screen_off_hint = 0;
static int screen_off_frames = 0;

static char toast_msg[80];
static float toast_t = 0;

/* resume */
static char resume_dir[384];
static int resume_side = 0, resume_track = 0;
static float resume_pos = 0;
static int resume_valid = 0;
static char resume_hint[128];

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void toast(const char *msg)
{
    strncpy(toast_msg, msg, sizeof toast_msg - 1);
    toast_msg[sizeof toast_msg - 1] = 0;
    toast_t = 2.0f;
}

static void sanitize(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 32 || (unsigned char)*s > 126) *s = '?';
}

static int ends_with_mp3(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && (strcasecmp(n + l - 4, ".mp3") == 0 || strcasecmp(n + l - 4, ".wav") == 0) && !(n[0] == '.' && n[1] == '_');
}

static int side_first(int s) { return s == 0 ? 0 : (ntracks + 1) / 2; }
static int side_count(int s) { return s == 0 ? (ntracks + 1) / 2 : ntracks - (ntracks + 1) / 2; }
static int side_last(int s) { return side_first(s) + side_count(s) - 1; }

static float track_dur(int i)
{
    return tracks[i].dur;
}

static void set_volume(void)
{
    float v = vol_step / 20.0f;
    player_set_volume((int)(0x8000 * v * v));
}

/* ------------------------------------------------------------------ */
/* resume file                                                         */

static void resume_path(char *out, int n) { snprintf(out, n, "%s/resume.txt", app_dir); }

static void save_settings_only(void);

static void save_resume(void)
{
    if (cur_tape < 0) { save_settings_only(); return; }
    char p[300];
    resume_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "dir=%s\nside=%d\ntrack=%d\npos=%.1f\nvol=%d\nautorev=%d\ntheme=%d\nboost=%d\n", tapes[cur_tape].dir, side,
            idx, pos, vol_step, auto_rev, theme, boost_i);
    fclose(f);
    strncpy(resume_dir, tapes[cur_tape].dir, sizeof resume_dir - 1);
    resume_side = side;
    resume_track = idx;
    resume_pos = pos;
    resume_valid = 1;
}

static void save_settings_only(void)
{
    char p[300];
    resume_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    if (resume_valid)
        fprintf(f, "dir=%s\nside=%d\ntrack=%d\npos=%.1f\n", resume_dir, resume_side, resume_track, resume_pos);
    fprintf(f, "vol=%d\nautorev=%d\ntheme=%d\nboost=%d\n", vol_step, auto_rev, theme, boost_i);
    fclose(f);
}

static void load_resume(void)
{
    char p[300], line[512];
    resume_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    resume_valid = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "dir=", 4)) {
            strncpy(resume_dir, line + 4, sizeof resume_dir - 1);
            resume_valid = 1;
        } else if (!strncmp(line, "side=", 5)) resume_side = atoi(line + 5);
        else if (!strncmp(line, "track=", 6)) resume_track = atoi(line + 6);
        else if (!strncmp(line, "pos=", 4)) resume_pos = (float)atof(line + 4);
        else if (!strncmp(line, "vol=", 4)) vol_step = atoi(line + 4);
        else if (!strncmp(line, "autorev=", 8)) auto_rev = atoi(line + 8);
        else if (!strncmp(line, "theme=", 6)) theme = atoi(line + 6);
        else if (!strncmp(line, "boost=", 6)) boost_i = atoi(line + 6);
    }
    fclose(f);
    if (vol_step < 0 || vol_step > 20) vol_step = 14;
    if (theme < 0 || theme >= NUM_THEMES) theme = 0;
    if (boost_i < 0 || boost_i > 4) boost_i = 0;
}

static int resume_tape_index(void)
{
    if (!resume_valid) return -1;
    for (int i = 0; i < ntapes; i++)
        if (!strcmp(tapes[i].dir, resume_dir)) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */

static void read_label(const char *dir, char *name, int n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/label.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) {
            strncpy(name, line, n - 1);
            name[n - 1] = 0;
        }
    }
    fclose(f);
}

static void scan_dir(const char *path, const char *label, int depth)
{
    SceUID d = sceIoDopen(path);
    if (d < 0) return;
    static char subs[4][MAX_SUBDIRS][128]; /* per depth level */
    int nsub = 0, count = 0;
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] != '.') {
            if (FIO_S_ISDIR(e.d_stat.st_mode)) {
                if (nsub < MAX_SUBDIRS && depth < 3) {
                    strncpy(subs[depth][nsub], e.d_name, 127);
                    subs[depth][nsub][127] = 0;
                    nsub++;
                }
            } else if (ends_with_mp3(e.d_name)) {
                count++;
            }
        }
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);

    if (count > 0 && ntapes < MAX_TAPES) {
        Tape *t = &tapes[ntapes];
        memset(t, 0, sizeof *t);
        strncpy(t->name, label, sizeof t->name - 1);
        strncpy(t->dir, path, sizeof t->dir - 1);
        read_label(path, t->name, sizeof t->name);
        sanitize(t->name);
        t->ntracks = count > MAX_TRACKS ? MAX_TRACKS : count;
        t->style = ui_style_for_name(t->name);
        ntapes++;
    }
    for (int i = 0; i < nsub && depth < 3; i++) {
        char child[512], clabel[128];
        snprintf(child, sizeof child, "%s/%s", path, subs[depth][i]);
        if (depth == 0) snprintf(clabel, sizeof clabel, "%s", subs[depth][i]);
        else snprintf(clabel, sizeof clabel, "%s - %s", label, subs[depth][i]);
        scan_dir(child, clabel, depth + 1);
    }
}

static int tape_cmp(const void *a, const void *b) { return strcasecmp(((const Tape *)a)->name, ((const Tape *)b)->name); }

static void scan_tapes(void)
{
    ntapes = 0;
    scan_dir("ms0:/MUSIC", "Loose tracks", 0);
    scan_dir("ms0:/PSP/MUSIC", "PSP Music", 0);
    qsort(tapes, ntapes, sizeof(Tape), tape_cmp);
    for (int i = 0; i < ntapes; i++) {
        uitapes[i].name = tapes[i].name;
        uitapes[i].ntracks = tapes[i].ntracks;
        uitapes[i].style = tapes[i].style;
        uitapes[i].total_sec = tapes[i].total;
    }
    if (rack_sel >= ntapes) rack_sel = ntapes > 0 ? ntapes - 1 : 0;
}

/* ------------------------------------------------------------------ */
/* display                                                             */

static uint32_t drawbuf[BUF_W * SCR_H] __attribute__((aligned(64)));
static int backbuf = 1;

static void present(int vblanks)
{
    uint32_t *vram = (uint32_t *)(0x44000000 + backbuf * BUF_W * SCR_H * 4);
    memcpy(vram, drawbuf, sizeof drawbuf);
    for (int i = 0; i < vblanks; i++) sceDisplayWaitVblankStart();
    sceDisplaySetFrameBuf(vram, BUF_W, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    backbuf ^= 1;
}

/* ------------------------------------------------------------------ */
/* tapes and tracks                                                    */

static int name_cmp(const void *a, const void *b)
{
    const Track *x = a, *y = b;
    return strcasecmp(x->path, y->path);
}

static void load_tape(int t)
{
    ntracks = 0;
    cur_tape = t;
    SceUID d = sceIoDopen(tapes[t].dir);
    if (d >= 0) {
        SceIoDirent e;
        memset(&e, 0, sizeof e);
        while (sceIoDread(d, &e) > 0 && ntracks < MAX_TRACKS) {
            if (e.d_name[0] != '.' && !FIO_S_ISDIR(e.d_stat.st_mode) && ends_with_mp3(e.d_name)) {
                Track *tr = &tracks[ntracks++];
                memset(tr, 0, sizeof *tr);
                snprintf(tr->path, sizeof tr->path, "%s/%s", tapes[t].dir, e.d_name);
            }
            memset(&e, 0, sizeof e);
        }
        sceIoDclose(d);
    }
    qsort(tracks, ntracks, sizeof(Track), name_cmp);

    float total = 0;
    for (int i = 0; i < ntracks; i++) {
        if ((i & 3) == 0) {
            char msg[80];
            snprintf(msg, sizeof msg, "Reading tape... %d / %d", i + 1, ntracks);
            ui_draw_loading(msg);
            present(1);
        }
        Track *tr = &tracks[i];
        Mp3Info mi;
        if (mp3_probe(tr->path, &mi) == 0) {
            tr->dur = mi.duration;
            strncpy(tr->title, mi.title, sizeof tr->title - 1);
            strncpy(tr->artist, mi.artist, sizeof tr->artist - 1);
        }
        if (!tr->title[0]) {
            /* filename without extension */
            const char *fn = strrchr(tr->path, '/');
            fn = fn ? fn + 1 : tr->path;
            strncpy(tr->title, fn, sizeof tr->title - 1);
            char *dot = strrchr(tr->title, '.');
            if (dot) *dot = 0;
            for (char *c = tr->title; *c; c++)
                if (*c == '_') *c = ' ';
        }
        sanitize(tr->title);
        sanitize(tr->artist);
        total += tr->dur;
    }
    tapes[t].total = total;
    uitapes[t].total_sec = total;
}

static void open_current(float sec, int play)
{
    if (ntracks == 0) return;
    sync_gen = player_status()->generation;
    player_open(tracks[idx].path, sec, !play);
    engine_loaded = 1;
    pos = sec;
}

static void start_play(void)
{
    const PlayerStatus *st = player_status();
    int valid = st->generation != sync_gen;
    if (engine_loaded && valid && st->state == PL_PAUSED) player_pause(0);
    else if (engine_loaded && !valid) player_pause(0); /* folds into the pending open */
    else open_current(pos, 1);
    mode = MODE_PLAY;
}

static void flip_side(void)
{
    side = 1 - side;
    idx = side_first(side);
    pos = 0;
    counter_val = 0;
    flip_t = 0.001f;
    char m[32];
    snprintf(m, sizeof m, "Side %c", side ? 'B' : 'A');
    toast(m);
}

static void end_of_side(void)
{
    if (auto_rev && side_count(1 - side) > 0) {
        flip_side();
        sfx_play(SFX_CLUNK);
        open_current(0, 1);
        mode = MODE_PLAY;
    } else {
        char m[40];
        snprintf(m, sizeof m, "End of side %c - rewound", side ? 'B' : 'A');
        toast(m);
        sfx_play(SFX_CLUNK);
        idx = side_first(side);
        mode = MODE_STOP;
        open_current(0, 0);
        counter_val = 0;
    }
    save_resume();
}

static void next_track_auto(void)
{
    if (idx < side_last(side)) {
        idx++;
        open_current(0, mode == MODE_PLAY);
        save_resume();
    } else {
        end_of_side();
    }
}

static void insert_tape(int t, int s, int track, float sec, int autoplay)
{
    load_tape(t);
    if (ntracks == 0) {
        toast("This tape is blank");
        cur_tape = -1;
        return;
    }
    if (s < 0 || s > 1 || side_count(s) == 0) s = 0;
    if (track < side_first(s) || track > side_last(s)) track = side_first(s);
    side = s;
    idx = track;
    mode = MODE_STOP;
    counter_val = 0;
    error_run = 0;
    open_current(sec, 0);
    screen = SCR_DECK;
    insert_t = 0;
    inserting = 1;
    ejecting = 0;
    play_after_insert = autoplay;
    sfx_play(SFX_CLICK);
}

static void eject(void)
{
    sfx_wind(0);
    save_resume();
    player_close();
    engine_loaded = 0;
    mode = MODE_STOP;
    ejecting = 1;
    inserting = 0;
    sfx_play(SFX_EJECT);
}

/* ------------------------------------------------------------------ */
/* callbacks                                                           */

static int exit_callback(int a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int th = sceKernelCreateThread("cb", callback_thread, 0x11, 0xFA0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
}

/* ------------------------------------------------------------------ */
/* input                                                               */

static unsigned int pad_now = 0, pad_old = 0;
static float rep_timer = 0;
static unsigned int rep_btn = 0;

static int pressed(unsigned int b) { return (pad_now & b) && !(pad_old & b); }
static int released(unsigned int b) { return !(pad_now & b) && (pad_old & b); }

/* returns 1 on press and then repeatedly while held */
static int repeat(unsigned int b, float dt)
{
    if (pressed(b)) {
        rep_btn = b;
        rep_timer = 0.35f;
        return 1;
    }
    if ((pad_now & b) && rep_btn == b) {
        rep_timer -= dt;
        if (rep_timer <= 0) {
            rep_timer = 0.07f;
            return 1;
        }
    }
    return 0;
}

static void handle_rack(float dt)
{
    if (ntapes > 0) {
        int old = rack_sel;
        if (repeat(PSP_CTRL_DOWN, dt)) rack_sel = (rack_sel + 1) % ntapes;
        if (repeat(PSP_CTRL_UP, dt)) rack_sel = (rack_sel + ntapes - 1) % ntapes;
        if (repeat(PSP_CTRL_RIGHT, dt)) rack_sel = rack_sel + 9 < ntapes ? rack_sel + 9 : ntapes - 1;
        if (repeat(PSP_CTRL_LEFT, dt)) rack_sel = rack_sel - 9 > 0 ? rack_sel - 9 : 0;
        if (old != rack_sel) {
            rack_slide = 0;
            sfx_play(SFX_CLICK);
        }
        if (pressed(PSP_CTRL_CROSS)) {
            insert_tape(rack_sel, 0, 0, 0, 1);
            return;
        }
        if (pressed(PSP_CTRL_TRIANGLE)) {
            int ri = resume_tape_index();
            if (ri >= 0) {
                rack_sel = ri;
                insert_tape(ri, resume_side, resume_track, resume_pos, 1);
                return;
            }
        }
    }
    if (pressed(PSP_CTRL_SELECT)) {
        theme = (theme + 1) % NUM_THEMES;
        ui_set_theme(theme);
        char m[48];
        snprintf(m, sizeof m, "Theme: %s", ui_theme_name(theme));
        toast(m);
        sfx_play(SFX_CLICK);
        save_resume();
    }
    if (pressed(PSP_CTRL_START)) {
        ui_draw_loading("Scanning ms0:/MUSIC ...");
        present(1);
        scan_tapes();
        char m[48];
        snprintf(m, sizeof m, "%d tape%s found", ntapes, ntapes == 1 ? "" : "s");
        toast(m);
    }
}

/* Fast forward / rewind: like a real deck, the music stops while the tape
   winds (with motor sound), speeding up the longer you hold. On release,
   playback continues from the new spot. */
static float cue_held = 0;

static void begin_cue(int dir)
{
    if (mode == MODE_FF || mode == MODE_REW || ntracks == 0) return;
    cue_prev_mode = mode;
    cue_dir = dir;
    cue_pos = pos;
    cue_held = 0;
    if (mode == MODE_PLAY) player_pause(1);
    mode = dir > 0 ? MODE_FF : MODE_REW;
    sfx_play(SFX_CLICK);
    sfx_wind(1);
}

static void end_cue(void)
{
    sfx_wind(0);
    mode = cue_prev_mode == MODE_PLAY ? MODE_PLAY : MODE_PAUSE;
    if (cue_prev_mode == MODE_STOP) mode = MODE_STOP;
    cue_dir = 0;
    pos = cue_pos;
    open_current(cue_pos, mode == MODE_PLAY);
    sfx_play(SFX_CLICK);
    save_resume();
}

static void update_cue(float dt)
{
    cue_held += dt;
    /* 12x at first, ramping up to 60x after about 3 seconds */
    float speed = 12.0f + (cue_held > 3.0f ? 3.0f : cue_held) * 16.0f;
    cue_pos += cue_dir * speed * dt;
    float d = track_dur(idx);
    if (d <= 0) d = 180.0f;
    if (cue_dir > 0 && cue_pos >= d - 0.3f) {
        if (idx < side_last(side)) {
            idx++;
            cue_pos = 0;
            sfx_play(SFX_CLICK);
        } else {
            cue_pos = d > 1.0f ? d - 1.0f : 0; /* end of the side: the tape stops */
        }
    } else if (cue_dir < 0 && cue_pos <= 0) {
        if (idx > side_first(side)) {
            idx--;
            float pd = track_dur(idx);
            if (pd <= 0) pd = 180.0f;
            cue_pos = pd - 0.3f;
            sfx_play(SFX_CLICK);
        } else {
            cue_pos = 0; /* start of the side */
        }
    }
    pos = cue_pos;
}

/* ---- shared actions (buttons, earphone remote and menu use these) ---- */

static void play_pause(void)
{
    if (mode == MODE_PLAY) {
        player_pause(1);
        mode = MODE_PAUSE;
        save_resume();
    } else {
        start_play();
    }
    sfx_play(SFX_CLUNK);
}

static void next_track(void)
{
    if (idx < side_last(side)) {
        idx++;
        open_current(0, mode == MODE_PLAY);
        sfx_play(SFX_CLICK);
    } else {
        toast("Last track on this side");
    }
}

static void prev_track(void)
{
    if (pos > 3.0f || idx == side_first(side)) open_current(0, mode == MODE_PLAY);
    else {
        idx--;
        open_current(0, mode == MODE_PLAY);
    }
    sfx_play(SFX_CLICK);
}

static void set_view(int v)
{
    view = v;
    screen_off = (v == VIEW_OFF);
    screen_off_hint = screen_off ? 3.0f : 0;
    screen_off_frames = 0;
    scePowerSetClockFrequency(screen_off ? 222 : 333, screen_off ? 222 : 333, screen_off ? 111 : 166);
    if (v == VIEW_CASSETTE) toast("Cassette view - SELECT to change");
}

static void set_boost(int i)
{
    boost_i = i;
    player_set_boost(boost_gain[i]);
}

static void set_sleep(int i)
{
    sleep_idx = i;
    sleep_left = sleep_opts[sleep_idx] > 0 ? sleep_opts[sleep_idx] * 60.0f : -1;
}

/* ---- options menu ---- */

enum { MI_THEME, MI_BOOST, MI_AUTOREV, MI_SLEEP, MI_VIEW, MI_CLOSE, MI_N };
static int menu_view = 0;
static char menu_vals[MI_N][32];

static void menu_fill(UiState *u)
{
    static const char *labels[MI_N] = {"Theme", "Volume boost", "Auto reverse", "Sleep timer", "View", "Close"};
    static const char *views[3] = {"Full deck", "Cassette only", "Screen off"};
    snprintf(menu_vals[MI_THEME], 32, "%s", ui_theme_name(theme));
    if (boost_i == 0) snprintf(menu_vals[MI_BOOST], 32, "Off");
    else snprintf(menu_vals[MI_BOOST], 32, "+%d dB", boost_db[boost_i]);
    snprintf(menu_vals[MI_AUTOREV], 32, "%s", auto_rev ? "On" : "Off");
    if (sleep_opts[sleep_idx] < 0) snprintf(menu_vals[MI_SLEEP], 32, "Off");
    else snprintf(menu_vals[MI_SLEEP], 32, "%d min", sleep_opts[sleep_idx]);
    snprintf(menu_vals[MI_VIEW], 32, "%s", views[menu_view]);
    menu_vals[MI_CLOSE][0] = 0;
    u->menu_open = menu_open;
    u->menu_sel = menu_sel;
    u->menu_n = MI_N;
    for (int k = 0; k < MI_N; k++) {
        u->menu_label[k] = labels[k];
        u->menu_value[k] = menu_vals[k];
    }
}

static void handle_menu(float dt)
{
    if (repeat(PSP_CTRL_DOWN, dt)) { menu_sel = (menu_sel + 1) % MI_N; sfx_play(SFX_CLICK); }
    if (repeat(PSP_CTRL_UP, dt)) { menu_sel = (menu_sel + MI_N - 1) % MI_N; sfx_play(SFX_CLICK); }
    int d = repeat(PSP_CTRL_RIGHT, dt) ? 1 : repeat(PSP_CTRL_LEFT, dt) ? -1 : 0;
    if (d) {
        switch (menu_sel) {
        case MI_THEME: theme = (theme + d + NUM_THEMES) % NUM_THEMES; ui_set_theme(theme); break;
        case MI_BOOST: set_boost((boost_i + d + 5) % 5); break;
        case MI_AUTOREV: auto_rev = !auto_rev; break;
        case MI_SLEEP: set_sleep((sleep_idx + d + 5) % 5); break;
        case MI_VIEW: menu_view = (menu_view + d + 3) % 3; break;
        }
        sfx_play(SFX_CLICK);
    }
    if (pressed(PSP_CTRL_CROSS)) {
        if (menu_sel == MI_VIEW || menu_sel == MI_CLOSE) {
            menu_open = 0;
            if (menu_sel == MI_VIEW) set_view(menu_view);
        } else if (menu_sel == MI_AUTOREV) auto_rev = !auto_rev;
        sfx_play(SFX_CLICK);
    }
    if (pressed(PSP_CTRL_CIRCLE) || pressed(PSP_CTRL_START)) { menu_open = 0; sfx_play(SFX_CLICK); }
    if (!menu_open) save_resume();
}

/* ---- the official PSP earphone remote ---- */

static void handle_remote(float dt)
{
    rm_old = rm_now;
    u32 k = 0;
    remote_present = sceHprmIsRemoteExist() > 0;
    if (remote_present) sceHprmPeekCurrentKey(&k);
    if (k & PSP_HPRM_HOLD) k = 0; /* hold switch on the remote */
    rm_now = k;
    int down_pp = (rm_now & PSP_HPRM_PLAYPAUSE) && !(rm_old & PSP_HPRM_PLAYPAUSE);
    if (screen == SCR_RACK) {
        if (down_pp && ntapes > 0) {
            int ri = resume_tape_index();
            if (ri >= 0) { rack_sel = ri; insert_tape(ri, resume_side, resume_track, resume_pos, 1); }
            else insert_tape(rack_sel, 0, 0, 0, 1);
        }
        return;
    }
    if (inserting || ejecting) return;
    if (down_pp) play_pause();
    /* forward / back: tap = change track, hold = wind */
    for (int dir = 1; dir >= -1; dir -= 2) {
        unsigned bit = dir > 0 ? PSP_HPRM_FORWARD : PSP_HPRM_BACK;
        float *t = dir > 0 ? &rm_fwd_t : &rm_back_t;
        if (rm_now & bit) {
            *t += dt;
            if (*t > 0.5f && !cue_dir) { begin_cue(dir); cue_src = 1; }
        } else if (rm_old & bit) {
            if (cue_dir && cue_src == 1) end_cue();
            else if (*t <= 0.5f) { if (dir > 0) next_track(); else prev_track(); }
            *t = 0;
        }
    }
}

static void handle_deck(float dt)
{
    if (inserting || ejecting) return;

    if (menu_open) { handle_menu(dt); return; }

    /* volume works in every mode, also with the screen off */
    if (repeat(PSP_CTRL_UP, dt) && vol_step < 20) { vol_step++; set_volume(); }
    if (repeat(PSP_CTRL_DOWN, dt) && vol_step > 0) { vol_step--; set_volume(); }

    if (pressed(PSP_CTRL_SELECT)) {
        set_view((view + 1) % 3); /* full deck -> cassette only -> screen off */
        sfx_play(SFX_CLICK);
    }
    if (pressed(PSP_CTRL_START)) {
        if (view == VIEW_OFF) set_view(VIEW_FULL);
        menu_open = 1;
        menu_sel = 0;
        menu_view = view;
        sfx_play(SFX_CLICK);
        return;
    }

    /* fast forward / rewind while held */
    if (pressed(PSP_CTRL_RTRIGGER)) { begin_cue(1); cue_src = 0; }
    if (pressed(PSP_CTRL_LTRIGGER)) { begin_cue(-1); cue_src = 0; }
    if (cue_src == 0 && cue_dir > 0 && released(PSP_CTRL_RTRIGGER)) end_cue();
    if (cue_src == 0 && cue_dir < 0 && released(PSP_CTRL_LTRIGGER)) end_cue();
    if (cue_dir) {
        update_cue(dt);
        return;
    }

    if (pressed(PSP_CTRL_CROSS)) play_pause();
    if (pressed(PSP_CTRL_CIRCLE)) {
        if (mode == MODE_PLAY || mode == MODE_PAUSE) {
            player_pause(1);
            mode = MODE_STOP;
            save_resume();
            sfx_play(SFX_CLUNK);
        } else {
            eject();
        }
    }
    if (pressed(PSP_CTRL_RIGHT)) next_track();
    if (pressed(PSP_CTRL_LEFT)) prev_track();
    if (pressed(PSP_CTRL_TRIANGLE)) {
        if (side_count(1 - side) == 0) {
            toast(side == 0 ? "Side B is blank" : "Side A is blank");
        } else {
            int was_playing = (mode == MODE_PLAY);
            flip_side();
            open_current(0, was_playing);
            mode = was_playing ? MODE_PLAY : MODE_STOP;
            sfx_play(SFX_CLUNK);
            save_resume();
        }
    }
    if (pressed(PSP_CTRL_SQUARE)) {
        set_sleep((sleep_idx + 1) % 5);
        char m[40];
        if (sleep_left > 0) snprintf(m, sizeof m, "Sleep timer: %d min", sleep_opts[sleep_idx]);
        else snprintf(m, sizeof m, "Sleep timer off");
        toast(m);
    }
}

/* ------------------------------------------------------------------ */
/* per-frame simulation                                                */

static void update_engine(void)
{
    const PlayerStatus *st = player_status();
    if (st->generation == sync_gen || !engine_loaded) return; /* waiting for our last command */
    if (mode == MODE_FF || mode == MODE_REW) return;
    if (mode == MODE_PLAY) {
        if (st->state == PL_PLAYING) {
            pos = st->pos;
            error_run = 0;
        } else if (st->state == PL_ENDED) {
            next_track_auto();
        } else if (st->state == PL_ERROR) {
            char m[80];
            snprintf(m, sizeof m, "Can't play: %.50s", tracks[idx].title);
            toast(m);
            if (++error_run > ntracks) {
                mode = MODE_STOP;
                player_pause(1);
            } else {
                next_track_auto();
            }
        }
    }
}

static float side_progress(void)
{
    float before = 0, total = 0;
    for (int i = side_first(side); i <= side_last(side); i++) {
        float d = track_dur(i);
        if (d <= 0) d = 180; /* unknown length: assume 3 minutes */
        if (i < idx) before += d;
        total += d;
    }
    if (total <= 0) return 0;
    float p = (before + pos) / total;
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

static void update_mechanics(float dt, float progress)
{
    /* tape transport speed with a little inertia */
    float target = 0;
    if (mode == MODE_PLAY) target = 1;
    else if (mode == MODE_FF) target = 9;
    else if (mode == MODE_REW) target = -9;
    if (inserting || ejecting) target = 0;
    float k = (fabsf(target) > fabsf(tape_speed)) ? 6.0f : 4.0f;
    tape_speed += (target - tape_speed) * (1 - expf(-k * dt));

    const float pmin = 5.4f, pmax = 14.5f, v = 7.0f;
    float rl = sqrtf(pmin * pmin + (pmax * pmax - pmin * pmin) * (1 - progress));
    float rr = sqrtf(pmin * pmin + (pmax * pmax - pmin * pmin) * progress);
    /* reels turn clockwise when playing (tape moves left to right) */
    angle_l += tape_speed * v * dt / rl;
    angle_r += tape_speed * v * dt / rr;
    if (angle_l > TWO_PI) angle_l -= TWO_PI;
    if (angle_l < 0) angle_l += TWO_PI;
    if (angle_r > TWO_PI) angle_r -= TWO_PI;
    if (angle_r < 0) angle_r += TWO_PI;
    counter_val += tape_speed * v * dt / rr / TWO_PI * 2.0f;
    if (counter_val < 0) counter_val += 1000;
    if (counter_val >= 1000) counter_val -= 1000;
}

static float level_to_needle(float level, int *peak)
{
    float dbfs = level > 1e-5f ? 20.0f * log10f(level) : -100.0f;
    float vu = dbfs + 14.0f; /* 0 VU at -14 dBFS RMS */
    *peak = vu > 1.5f;
    float t = (vu + 20.0f) / 23.0f;
    if (t < 0) t = 0;
    if (t > 1.1f) t = 1.1f;
    return powf(t, 1.6f);
}

static void update_vu(float dt)
{
    const PlayerStatus *st = player_status();
    int pl = 0, pr = 0;
    float tl = 0, tr = 0;
    if (mode == MODE_PLAY || mode == MODE_FF || mode == MODE_REW) {
        tl = level_to_needle(st->level_l, &pl);
        tr = level_to_needle(st->level_r, &pr);
    }
    /* needle as a damped spring: a little overshoot like the real thing */
    const float K = 170.0f, D = 19.0f;
    float steps = dt / 0.004f;
    int n = (int)steps + 1;
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        vu_vl += ((tl - vu_l) * K - vu_vl * D) * h;
        vu_vr += ((tr - vu_r) * K - vu_vr * D) * h;
        vu_l += vu_vl * h;
        vu_r += vu_vr * h;
    }
    if (vu_l < 0) { vu_l = 0; vu_vl = 0; }
    if (vu_r < 0) { vu_r = 0; vu_vr = 0; }
    if (pl) peak_l_t = 0.25f;
    if (pr) peak_r_t = 0.25f;
    peak_l_t -= dt;
    peak_r_t -= dt;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);

    if (argc > 0 && argv[0]) {
        strncpy(app_dir, argv[0], sizeof app_dir - 1);
        char *sl = strrchr(app_dir, '/');
        if (sl) *sl = 0;
    }

    sceDisplaySetMode(0, SCR_W, SCR_H);
    gfx_set_target(drawbuf);
    ui_init();
    ui_draw_loading("Loading...");
    present(1);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    int perr = player_init();
    load_resume();
    ui_set_theme(theme);
    player_set_boost(boost_gain[boost_i]);
    set_volume();
    scan_tapes();

    if (perr < 0) {
        char l1[64];
        snprintf(l1, sizeof l1, "MP3 decoder failed to start (0x%08X).", (unsigned)perr);
        while (running) {
            ui_draw_message("Audio error", l1, "Try running from the XMB Game menu", "Press HOME to quit.");
            present(2);
        }
        sceKernelExitGame();
        return 0;
    }

    SceInt64 last = sceKernelGetSystemTimeWide();
    float clock_t = 0, tick_t = 0, now_sec = 0;
    int bat = -1, chg = 0, hh = -1, mm = 0;

    while (running) {
        SceInt64 now = sceKernelGetSystemTimeWide();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        now_sec += dt;

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        pad_old = pad_now;
        pad_now = pad.Buttons;

        /* status info once per second */
        clock_t -= dt;
        if (clock_t <= 0) {
            clock_t = 1.0f;
            ScePspDateTime tm;
            if (sceRtcGetCurrentClockLocalTime(&tm) >= 0) {
                hh = tm.hour;
                mm = tm.minute;
            }
            bat = scePowerIsBatteryExist() ? scePowerGetBatteryLifePercent() : -1;
            chg = scePowerIsBatteryCharging();
        }
        /* keep the PSP from auto-sleeping while music plays */
        tick_t -= dt;
        if (tick_t <= 0) {
            tick_t = 5.0f;
            if (mode == MODE_PLAY) scePowerTick(PSP_POWER_TICK_SUSPEND);
        }
        if (toast_t > 0) toast_t -= dt;

        /* sleep timer */
        if (sleep_left > 0) {
            sleep_left -= dt;
            if (sleep_left <= 0) {
                sleep_left = -1;
                sleep_idx = 0;
                if (mode == MODE_PLAY) {
                    player_pause(1);
                    mode = MODE_PAUSE;
                    sfx_play(SFX_CLUNK);
                }
                save_resume();
                toast("Sleep timer: goodnight");
            }
        }

        UiState s;
        memset(&s, 0, sizeof s);
        s.battery = bat;
        s.charging = chg;
        s.hour = hh;
        s.minute = mm;
        s.toast = toast_msg;
        s.toast_alpha = toast_t > 0.4f ? 1.0f : (toast_t > 0 ? toast_t / 0.4f : 0);
        s.time_sec = now_sec;

        handle_remote(dt);
        if (screen == SCR_RACK) {
            handle_rack(dt);
        } else {
            handle_deck(dt);
            update_engine();
        }

        if (screen == SCR_RACK) {
            /* smooth scrolling list */
            if (rack_sel < rack_scroll_target) rack_scroll_target = (float)rack_sel;
            if (rack_sel > rack_scroll_target + 8) rack_scroll_target = (float)(rack_sel - 8);
            rack_scroll += (rack_scroll_target - rack_scroll) * (1 - expf(-14 * dt));
            rack_slide += (1 - rack_slide) * (1 - expf(-12 * dt));
            int ri = resume_tape_index();
            if (ri >= 0) {
                snprintf(resume_hint, sizeof resume_hint, "Resume: %s, side %c", tapes[ri].name, resume_side ? 'B' : 'A');
                s.resume_hint = resume_hint;
            }
            s.tapes = uitapes;
            s.ntapes = ntapes;
            s.sel = rack_sel;
            s.scroll = rack_scroll;
            s.sel_slide = rack_slide;
            ui_draw_rack(&s);
            present(2);
            continue;
        }

        /* deck animations */
        if (inserting) {
            insert_t += dt / 0.45f;
            if (insert_t >= 1) {
                insert_t = 1;
                inserting = 0;
                sfx_play(SFX_CLUNK);
                if (play_after_insert) start_play();
            }
        }
        if (ejecting) {
            insert_t -= dt / 0.4f;
            if (insert_t <= 0) {
                insert_t = 0;
                ejecting = 0;
                screen = SCR_RACK;
                rack_slide = 0;
                if (view != VIEW_FULL) set_view(VIEW_FULL);
                menu_open = 0;
                continue;
            }
        }
        if (flip_t > 0) {
            flip_t += dt / 0.5f;
            if (flip_t >= 1) flip_t = 0;
        }

        float prog = side_progress();
        update_mechanics(dt, prog);
        update_vu(dt);

        if (screen_off) {
            screen_off_hint -= dt;
            s.toast_alpha = screen_off_hint > 0 ? (screen_off_hint > 1 ? 1 : screen_off_hint) : 0;
            /* draw until both buffers are black, then just idle */
            if (screen_off_hint > -0.2f || screen_off_frames < 2) {
                ui_draw_screen_off(&s);
                present(2);
                if (screen_off_hint <= 0) screen_off_frames++;
            } else {
                sceDisplayWaitVblankStart();
                sceDisplayWaitVblankStart();
            }
            continue;
        }

        Track *tr = ntracks > 0 ? &tracks[idx] : NULL;
        s.tape_name = cur_tape >= 0 ? tapes[cur_tape].name : NULL;
        s.tape_style = cur_tape >= 0 ? tapes[cur_tape].style : 0;
        s.side = side ? 'B' : 'A';
        s.track_no = idx - side_first(side) + 1;
        s.tracks_on_side = side_count(side);
        s.title = tr ? tr->title : NULL;
        s.artist = tr ? tr->artist : NULL;
        s.pos = pos;
        s.dur = track_dur(idx);
        s.side_progress = prog;
        s.mode = mode;
        s.angle_l = angle_l;
        s.angle_r = angle_r;
        s.counter = counter_val;
        s.vu_l = vu_l;
        s.vu_r = vu_r;
        s.peak_l = peak_l_t > 0;
        s.peak_r = peak_r_t > 0;
        s.volume = vol_step;
        s.sleep_min = sleep_left > 0 ? (int)ceilf(sleep_left / 60.0f) : -1;
        s.auto_reverse = auto_rev;
        s.flip = flip_t;
        s.insert = insert_t;
        s.boost_db = boost_db[boost_i];
        s.remote = remote_present;
        menu_fill(&s);
        if (view == VIEW_CASSETTE) ui_draw_cassette_view(&s);
        else ui_draw_deck(&s);
        ui_draw_menu(&s);
        present(2);
    }

    save_resume();
    player_shutdown();
    sceKernelExitGame();
    return 0;
}
