/*
 * The circular drum machine interface, laid out from the hand-drawn sketch:
 *
 *   DIV rail (8)   concentric ring sequencer   FULL / KILL (8)
 *   ---------------------------------------------------------
 *   LENGTH / BPM   8 sample slots, 8 round pads   SPYRO 24
 *
 * The whole layout is 880x500. Outside the ring a long tick marks each bar
 * and a short one each beat, so the loop is divided even with nothing loaded.
 *
 * Timing model
 * ------------
 * LENGTH is a number of bars. Every track has two typed rates:
 *
 *   DIV  - how often the track's sample fires (a note value: 1/4, 1/8, 1/3...).
 *          The ring shows that track's slots at its DIV rate; a slot is on
 *          when the sample should fire on it.
 *   KILL - a second rate. Wherever a KILL hit lands on exactly the same tick
 *          as a DIV hit, the sample does not sound: the slot is drawn grey.
 *          KILL's own OFFSET rotates that grid the same way DIV's does.
 *
 * MUTE, on the left rail, silences a track without touching its level: the
 * fader keeps its value and comes back exactly as it was.
 *
 * Positions are integer 1/96-note ticks, which represents every rate down to
 * 1/32 as well as triplets exactly, so the coincidence test is exact:
 * two hits collide when the DIV tick is a whole multiple of the KILL tick.
 *
 * DIV and KILL boxes are text fields: click one and type. "1/8" sets that
 * ratio, and a bare number "3" is shorthand for 1/3. Enter commits and moves
 * to the next row, Esc reverts.
 *
 * The rings are a display of those two rates, not an editor: every DIV position
 * inside the loop is a hit (amber) unless a KILL starts on that same tick
 * (grey). A click on a ring only selects its track. A KILL of 0 switches the
 * kill off for that track entirely.
 *
 * OFFSET rotates a track's grid inside the loop by a typed note value, so the
 * hits keep their spacing but start where the offset puts them. It is always
 * shown as a ratio (0/1 when there is no offset). FULL decides
 * how far a KILL reaches: off, it cancels only the hit it starts on; on, it
 * cancels any hit whose own step a KILL start falls inside.
 *
 * The eight pads under the slots are the track levels: drag one up or down (or
 * scroll it) to set that track's volume anywhere from -inf dB to +10 dB. A pad
 * shows the value and draws its position as a ring.
 *
 * Ring n (inside out) is track n, matching the DIV and KILL rows top to
 * bottom. Each of the eight slot buttons loads a .wav or .mp3 into its track,
 * either through the built-in browser or by dropping a file onto it from the
 * file manager, and shows [LOAD] until it has one; hovering a button lights up
 * that track's ring.
 */
#include "ui.h"
#include "audio.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAU 6.28318530718f

#define CANVAS_W 880.0f
#define CANVAS_H 500.0f

#define TICKS_PER_BAR 96 /* 1/96 note: 1/4 = 24, 1/8 = 12, 1/3 = 32, 1/16 = 6 */
#define TICKS_PER_BEAT 24
#define MAX_CELLS 256    /* 8 bars at 1/32 */

#define SAMPLE_PATH_MAX 512
#define SAMPLE_LABEL_MAX 32
#define BROWSER_MAX 512
#define BROWSER_W 560.0f
#define BROWSER_H 420.0f
#define LIST_ROW_H 26.0f
#define LIST_ROWS 10

#define EDIT_NONE (-1)
#define EDIT_DIV(i) (i)
#define EDIT_KILL(i) (8 + (i))
#define EDIT_OFF(i) (16 + (i))
#define EDIT_KOFF(i) (24 + (i))

/* ------------------------------------------------------------------ palette */

#define COL_BG       CDM_RGBA(17, 19, 23, 255)
#define COL_BAND     CDM_RGBA(20, 22, 27, 255)
#define COL_RULE     CDM_RGBA(38, 44, 54, 255)
#define COL_BTN      CDM_RGBA(28, 32, 40, 255)
#define COL_BTN_SEL  CDM_RGBA(40, 36, 30, 255)
#define COL_STROKE   CDM_RGBA(56, 64, 78, 255)
#define COL_TEXT     CDM_RGBA(228, 233, 242, 255)
#define COL_DIM      CDM_RGBA(120, 130, 146, 255)
#define COL_FAINT    CDM_RGBA(46, 52, 63, 255)
#define COL_RING     CDM_RGBA(58, 66, 79, 255)
#define COL_ACCENT   CDM_RGBA(245, 167, 66, 255)
#define COL_ACC_SOFT CDM_RGBA(245, 167, 66, 90)
#define COL_ACC_TINT CDM_RGBA(245, 167, 66, 16)
#define COL_ACC_HOVER CDM_RGBA(245, 167, 66, 30)
#define COL_ACC_HI   CDM_RGBA(255, 214, 150, 255)
#define COL_KILL     CDM_RGBA(226, 86, 74, 255)
#define COL_KILL_DIM CDM_RGBA(150, 78, 72, 255)
#define COL_KILLED   CDM_RGBA(122, 126, 134, 120)
#define COL_TICK     CDM_RGBA(255, 255, 255, 96)
#define COL_BRAND    CDM_RGBA(78, 87, 103, 255)
#define COL_SHADE    CDM_RGBA(6, 7, 9, 170)
#define COL_PANEL    CDM_RGBA(24, 27, 33, 255)
#define COL_ROW      CDM_RGBA(34, 39, 48, 255)
#define COL_FOLDER   CDM_RGBA(130, 168, 220, 255)

/* -------------------------------------------------------------------- model */

static const int BARS_VALUES[4] = { 1, 2, 4, 8 };
/* Ratios offered when scrolling a box; typing accepts anything valid. */
static const int STD_NUM[7] = { 1, 1, 1, 1, 1, 1, 1 };
static const int STD_DEN[7] = { 1, 2, 3, 4, 6, 8, 16 };

typedef struct {
    float x, y, w, h;
} box_t;

typedef struct {
    char name[192];
    int is_dir;
} entry_t;

struct ui {
    font_t *bold;
    font_t *reg;

    int bars;
    int playing;
    float bpm;
    double beats;

    /* Set by the VST3 wrapper: a host owns the clock while it is rolling. */
    int host_attached;
    int host_playing;
    double host_beats;

    int div_num[8], div_den[8];
    int kill_num[8], kill_den[8];
    int off_num[8], off_den[8];   /* start offset as a note value; 0 = none */
    int koff_num[8], koff_den[8]; /* the same, for the kill grid */
    int full[8];                /* KILL covers the hit's whole step */
    int mute[8];                /* silent, but the level is kept */
    float vol[8];               /* track level: 0 = -inf, unity at 0 dB */
    int sel;

    int edit_field; /* EDIT_NONE or one of the EDIT_* field ids */
    char edit_buf[8];

    float trig[8];
    int prev_cell[8];

    char smp_path[8][SAMPLE_PATH_MAX];
    char smp_label[8][SAMPLE_LABEL_MAX];
    int hover; /* sample slot under the cursor, -1 when none */

    int dragging;
    float drag_y0;
    float drag_bpm;

    int vol_drag; /* pad whose level is being dragged, -1 when none */
    float vol_y0, vol_p0;

    int browser_open;
    int browser_track;
    int browser_hover;
    int browser_scroll;
    int entry_n;
    char browser_dir[SAMPLE_PATH_MAX];
    entry_t entry[BROWSER_MAX];

    int dirty;
    int kit_dirty; /* the engine's copy of the pattern needs refreshing */
    mixer_t *mix;

    /* layout, rebuilt every frame and reused for hit testing */
    box_t div_btn[8];
    box_t kill_btn[8];
    box_t off_btn[8];
    box_t koff_btn[8];
    box_t mute_btn[8];
    box_t full_btn[8];
    box_t slot_btn[8];
    float pad_x[8];
    float pad_y[8];
    box_t len_box;
    box_t bpm_box;
    box_t play_box;
    float rcx, rcy, r0, ring_w, r_out;
};

#define PAD_R 19.0f

/* Pad travel is a dB fader: silent at the bottom, +10 dB at the top. */
#define VOL_OFF_POS 0.04f
#define VOL_UNITY_POS (2.0f / 3.0f) /* 0 dB sits two thirds of the way up */
#define VOL_MIN_DB (-60.0f)
#define VOL_MAX_DB (10.0f)

/* ------------------------------------------------------------------ helpers */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Pad positions run 0 (silent) to 1 (+10 dB), with 0 dB at VOL_UNITY_POS:
 * the lower two thirds attenuate, the top third is the boost. */
static float vol_db(float p)
{
    if (p <= VOL_OFF_POS)
        return VOL_MIN_DB;
    if (p <= VOL_UNITY_POS)
        return VOL_MIN_DB +
               (p - VOL_OFF_POS) / (VOL_UNITY_POS - VOL_OFF_POS) * (0.0f - VOL_MIN_DB);
    return (p - VOL_UNITY_POS) / (1.0f - VOL_UNITY_POS) * VOL_MAX_DB;
}

static float vol_gain(float p)
{
    return p <= VOL_OFF_POS ? 0.0f : powf(10.0f, vol_db(p) / 20.0f);
}

static float vol_pos_of_db(float db)
{
    if (db <= VOL_MIN_DB)
        return 0.0f;
    if (db <= 0.0f)
        return VOL_OFF_POS +
               (db - VOL_MIN_DB) / (0.0f - VOL_MIN_DB) * (VOL_UNITY_POS - VOL_OFF_POS);
    return VOL_UNITY_POS + (db / VOL_MAX_DB) * (1.0f - VOL_UNITY_POS);
}

/* The level ring on a pad starts at the bottom of the dial and sweeps
 * clockwise, so a pad at full level closes into a complete circle. */
static void draw_vol_ring(canvas_t *c, float x, float y, float p, uint32_t col)
{
    float r_in = PAD_R - 5.0f, r_out = PAD_R - 2.0f;
    float a0 = TAU * 0.5f; /* 6 o'clock */
    float a1 = a0 + p * TAU;
    if (a1 <= TAU) {
        d_arc(c, x, y, r_in, r_out, a0, a1, col);
    } else {
        d_arc(c, x, y, r_in, r_out, a0, TAU, col);
        d_arc(c, x, y, r_in, r_out, 0.0f, a1 - TAU, col);
    }
}

static void format_vol(float p, char *out, size_t cap)
{
    if (p <= VOL_OFF_POS) {
        snprintf(out, cap, "-inf");
        return;
    }
    int db = (int)lroundf(vol_db(p));
    if (db == 0)
        snprintf(out, cap, "0");
    else
        snprintf(out, cap, "%+d", db);
}

static int mark(ui_t *u)
{
    u->dirty = 1;
    u->kit_dirty = 1;
    return 1;
}

static int in_box(box_t b, float x, float y)
{
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

/* A track with no sample has nothing to play, so its whole row reads as off. */
static int track_loaded(const ui_t *u, int v)
{
    return u->smp_label[v][0] != '\0';
}

/* --------------------------------------------------------------- timing math */

/* Ticks per hit for the ratio num/den, in 1/96 notes. */
static int ratio_ticks(int num, int den)
{
    if (num < 1 || den < 1)
        return TICKS_PER_BAR;
    int t = (TICKS_PER_BAR * num + den / 2) / den;
    if (t < 1)
        t = 1;
    if (t > TICKS_PER_BAR * 8)
        t = TICKS_PER_BAR * 8;
    return t;
}

static int div_ticks(const ui_t *u, int v)
{
    return ratio_ticks(u->div_num[v], u->div_den[v]);
}

/* 0 means the track has no kill at all. */
static int kill_ticks(const ui_t *u, int v)
{
    if (u->kill_num[v] <= 0)
        return 0;
    return ratio_ticks(u->kill_num[v], u->kill_den[v]);
}

/* Phase of the KILL grid inside its own step, the way OFFSET rotates DIV. */
static int kill_off_ticks(const ui_t *u, int v)
{
    int kt = kill_ticks(u, v);
    if (u->koff_num[v] <= 0 || kt <= 0)
        return 0;
    return ratio_ticks(u->koff_num[v], u->koff_den[v]) % kt;
}

static int loop_ticks(const ui_t *u)
{
    return u->bars * TICKS_PER_BAR;
}

/* The track's hits start this many ticks into the loop. */
static int off_ticks(const ui_t *u, int v)
{
    int loop = loop_ticks(u);
    if (u->off_num[v] <= 0 || loop <= 0)
        return 0;
    return ratio_ticks(u->off_num[v], u->off_den[v]) % loop;
}

/* Where the first hit sits inside the DIV grid; the offset only rotates it. */
static int phase_ticks(const ui_t *u, int v)
{
    int dt = div_ticks(u, v);
    return dt > 0 ? off_ticks(u, v) % dt : 0;
}

/* Hits sit at phase + k*div and wrap past the loop point, so an offset turns
 * the grid instead of leaving an empty slice at the start of the loop. */
static int cell_count(const ui_t *u, int v)
{
    int dt = div_ticks(u, v);
    int span = loop_ticks(u) - phase_ticks(u, v);
    if (span < 1)
        span = 1;
    int c = (span + dt - 1) / dt;
    if (c < 1)
        c = 1;
    if (c > MAX_CELLS)
        c = MAX_CELLS;
    return c;
}

static int cell_start(const ui_t *u, int v, int s)
{
    return phase_ticks(u, v) + s * div_ticks(u, v);
}

/* Position inside the loop, in 1/96-note ticks. */
static double loop_pos(const ui_t *u)
{
    double len = (double)loop_ticks(u);
    double p = fmod(u->beats * (double)TICKS_PER_BEAT, len);
    if (p < 0.0)
        p += len;
    return p;
}

static int current_cell(const ui_t *u, int v)
{
    double rel = loop_pos(u) - (double)phase_ticks(u, v);
    if (rel < 0.0)
        rel += (double)loop_ticks(u); /* the first cell of the loop wraps */
    int c = (int)(rel / (double)div_ticks(u, v));
    int cc = cell_count(u, v);
    if (c < 0)
        c = 0;
    if (c >= cc)
        c = cc - 1;
    return c;
}

/*
 * A DIV hit is cancelled when a KILL hit starts on exactly the same tick.
 * With FULL switched on the kill uses its whole range instead: any KILL start
 * inside the hit's own DIV step cancels it, which matters when the kill grid is
 * finer than the DIV grid and the two do not line up.
 */
static int cell_killed(const ui_t *u, int v, int cell)
{
    int start = cell_start(u, v, cell);
    int loop = loop_ticks(u);
    int kt = kill_ticks(u, v);
    if (start >= loop || start < 0 || kt <= 0)
        return 0;
    int kp = kill_off_ticks(u, v);
    if (start % kt == kp)
        return 1; /* a kill starts on the hit itself */
    if (!u->full[v])
        return 0;
    /* No clamp at the loop end: the kill grid repeats, so a step that runs past
     * the last tick still meets the kill sitting on the next loop's tick 0. */
    int last = start + div_ticks(u, v) - 1;
    int next = start + (((kp - start) % kt) + kt) % kt;
    return next <= last; /* a kill starts inside the step */
}

static int track_has_kills(const ui_t *u, int v)
{
    int cc = cell_count(u, v);
    for (int s = 0; s < cc; s++)
        if (cell_killed(u, v, s))
            return 1;
    return 0;
}

static void format_ratio(int num, int den, char *out, size_t cap)
{
    snprintf(out, cap, "%d/%d", num, den);
}

/* KILL and OFFSET use 0 for "off", which is shown as a plain 0 rather than 0/1. */
static void format_num_or_zero(int num, int den, char *out, size_t cap)
{
    if (num <= 0)
        snprintf(out, cap, "0");
    else
        format_ratio(num, den, out, cap);
}

/* OFFSET always reads as a ratio, so no offset is 0/1 rather than a lone 0. */
static void format_offset(const ui_t *u, int v, char *out, size_t cap)
{
    format_ratio(u->off_num[v], u->off_den[v], out, cap);
}

static void layout(ui_t *u)
{
    const float rail_w = 64.0f;
    const float rail_l = 20.0f;
    const float rail_r = CANVAS_W - 20.0f - rail_w;
    const float row_h = 22.0f;
    const float row_gap = 7.0f;
    const float row_y0 = 38.0f;

    for (int i = 0; i < 8; i++) {
        float y = row_y0 + (float)i * (row_h + row_gap);
        u->div_btn[i] = (box_t){ rail_l, y, rail_w, row_h };
        u->kill_btn[i] = (box_t){ rail_r, y, rail_w, row_h };
        u->off_btn[i] = (box_t){ rail_l + rail_w + 26.0f, y, rail_w, row_h };
        u->full_btn[i] = (box_t){ rail_r - 2.0f * (rail_w + 18.0f), y, rail_w, row_h };
        u->koff_btn[i] = (box_t){ rail_r - (rail_w + 18.0f), y, rail_w, row_h };
        u->mute_btn[i] = (box_t){ rail_l + 2.0f * (rail_w + 26.0f), y, rail_w, row_h };
    }

    u->rcx = 440.0f;
    u->rcy = 141.0f;
    u->r0 = 20.0f;
    u->r_out = 124.0f;
    u->ring_w = (u->r_out - u->r0) / 8.0f;

    u->len_box = (box_t){ 20.0f, 304.0f, 64.0f, 26.0f };
    u->bpm_box = (box_t){ 20.0f, 352.0f, 64.0f, 34.0f };
    u->play_box = (box_t){ 20.0f, 436.0f, 64.0f, 48.0f };

    const float gx0 = 108.0f;
    const float gw = 148.0f;
    const float gp = 164.0f;
    for (int i = 0; i < 4; i++) {
        float x = gx0 + (float)i * gp;
        u->slot_btn[i] = (box_t){ x, 300.0f, gw, 27.0f };
        u->slot_btn[i + 4] = (box_t){ x, 332.0f, gw, 27.0f };
        u->pad_x[i] = x + gw * 0.5f;
        u->pad_x[i + 4] = x + gw * 0.5f;
        u->pad_y[i] = 384.0f;
        u->pad_y[i + 4] = 438.0f;
    }
}

/* ------------------------------------------------------------ sample browser */

static void path_dir(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    if (slash == out) {
        out[1] = '\0';
        return;
    }
    *slash = '\0';
}

static void path_join(char *out, size_t cap, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, cap, "/%s", name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

static void sample_label(const char *path, char *out, size_t cap)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char tmp[192];
    size_t blen = strlen(base);
    if (blen >= sizeof tmp)
        blen = sizeof tmp - 1;
    memcpy(tmp, base, blen);
    tmp[blen] = '\0';

    char *dot = strrchr(tmp, '.');
    if (dot && dot != tmp)
        *dot = '\0';

    size_t len = strlen(tmp);
    if (len > 15) {
        memcpy(out, tmp, 12);
        memcpy(out + 12, "...", 3);
        out[15] = '\0';
    } else if (len + 1 <= cap) {
        memcpy(out, tmp, len + 1);
    } else {
        memcpy(out, tmp, cap - 1);
        out[cap - 1] = '\0';
    }
}

/* Every place a sample can be chosen accepts these; the audio engine will
 * decode them once it exists. */
static int is_audio_file(const char *n)
{
    static const char *ext[3] = { ".wav", ".wave", ".mp3" };
    size_t len = strlen(n);
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(ext[i]);
        if (len >= l && strcasecmp(n + len - l, ext[i]) == 0)
            return 1;
    }
    return 0;
}

static int entry_cmp(const void *a, const void *b)
{
    const entry_t *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

static int load_dir(ui_t *u, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int n = 0;
    if (strcmp(dir, "/") != 0) {
        snprintf(u->entry[0].name, sizeof u->entry[0].name, "..");
        u->entry[0].is_dir = 1;
        n = 1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < BROWSER_MAX) {
        if (de->d_name[0] == '.')
            continue;
        char full[SAMPLE_PATH_MAX];
        path_join(full, sizeof full, dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        if (!is_dir && !is_audio_file(de->d_name))
            continue;
        size_t nl = strlen(de->d_name);
        if (nl >= sizeof u->entry[n].name)
            nl = sizeof u->entry[n].name - 1;
        memcpy(u->entry[n].name, de->d_name, nl);
        u->entry[n].name[nl] = '\0';
        u->entry[n].is_dir = is_dir;
        n++;
    }
    closedir(d);

    if (n > 1)
        qsort(u->entry + 1, (size_t)(n - 1), sizeof u->entry[0], entry_cmp);

    u->entry_n = n;
    u->browser_scroll = 0;
    u->browser_hover = -1;
    snprintf(u->browser_dir, sizeof u->browser_dir, "%s", dir);
    return 1;
}

static void browser_open_for(ui_t *u, int track)
{
    char start[SAMPLE_PATH_MAX] = { 0 };

    if (u->smp_path[track][0])
        path_dir(u->smp_path[track], start, sizeof start);
    else if (u->browser_dir[0])
        snprintf(start, sizeof start, "%s", u->browser_dir);
    else if (!getcwd(start, sizeof start))
        snprintf(start, sizeof start, ".");

    if (!load_dir(u, start)) {
        const char *home = getenv("HOME");
        if (!load_dir(u, home ? home : "."))
            return;
    }
    u->browser_open = 1;
    u->browser_track = track;
}

static void browser_choose(ui_t *u, int idx)
{
    if (idx < 0 || idx >= u->entry_n)
        return;

    if (strcmp(u->entry[idx].name, "..") == 0) {
        char parent[SAMPLE_PATH_MAX];
        path_dir(u->browser_dir, parent, sizeof parent);
        load_dir(u, parent);
        return;
    }

    char full[SAMPLE_PATH_MAX];
    path_join(full, sizeof full, u->browser_dir, u->entry[idx].name);

    if (u->entry[idx].is_dir) {
        load_dir(u, full);
        return;
    }

    int t = u->browser_track;
    snprintf(u->smp_path[t], sizeof u->smp_path[t], "%s", full);
    sample_label(full, u->smp_label[t], sizeof u->smp_label[t]);
    if (u->mix)
        mixer_load(u->mix, t, full);
    u->sel = t;
    u->browser_open = 0;
}

static box_t browser_box(void)
{
    return (box_t){ (CANVAS_W - BROWSER_W) * 0.5f, (CANVAS_H - BROWSER_H) * 0.5f,
                    BROWSER_W, BROWSER_H };
}

static box_t browser_list_box(void)
{
    box_t b = browser_box();
    return (box_t){ b.x + 16.0f, b.y + 96.0f, b.w - 42.0f, LIST_ROW_H * LIST_ROWS };
}

/* --------------------------------------------------------------- ratio edits */

static int parse_ratio(const char *s, int *num, int *den)
{
    if (!s || !*s)
        return 0;

    int a = 0, b = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9')
        a = a * 10 + (*p++ - '0');
    if (a <= 0)
        return 0;

    if (*p == '/') {
        p++;
        if (*p < '0' || *p > '9')
            return 0;
        while (*p >= '0' && *p <= '9')
            b = b * 10 + (*p++ - '0');
        if (b <= 0)
            return 0;
    } else if (*p == '\0') {
        b = a; /* "8" is shorthand for 1/8 */
        a = 1;
    } else {
        return 0;
    }
    if (*p != '\0' || a > 64 || b > 128)
        return 0;

    *num = a;
    *den = b;
    return 1;
}

static void ratio_of(const ui_t *u, int field, int *num, int *den)
{
    int i = field & 7;
    if (field < 8) {
        *num = u->div_num[i];
        *den = u->div_den[i];
    } else if (field < 16) {
        *num = u->kill_num[i];
        *den = u->kill_den[i];
    } else if (field < 24) {
        *num = u->off_num[i];
        *den = u->off_den[i];
    } else {
        *num = u->koff_num[i];
        *den = u->koff_den[i];
    }
}

static void ratio_set(ui_t *u, int field, int num, int den)
{
    int i = field & 7;
    if (field < 8) {
        u->div_num[i] = num;
        u->div_den[i] = den;
    } else if (field < 16) {
        u->kill_num[i] = num;
        u->kill_den[i] = den;
    } else if (field < 24) {
        u->off_num[i] = num;
        u->off_den[i] = den;
    } else {
        u->koff_num[i] = num;
        u->koff_den[i] = den;
    }
    u->prev_cell[i] = current_cell(u, i);
}

static int edit_start(ui_t *u, int field)
{
    u->edit_field = field;
    u->edit_buf[0] = '\0';
    return mark(u);
}

static int edit_cancel(ui_t *u)
{
    if (u->edit_field == EDIT_NONE)
        return 0;
    u->edit_field = EDIT_NONE;
    u->edit_buf[0] = '\0';
    return mark(u);
}

static int edit_commit(ui_t *u, int advance)
{
    if (u->edit_field == EDIT_NONE)
        return 0;

    int num, den;
    int field = u->edit_field;

    int ok = parse_ratio(u->edit_buf, &num, &den);
    if (!ok && field >= EDIT_KILL(0) && strcmp(u->edit_buf, "0") == 0) {
        num = 0; /* KILL 0 switches the kill off, OFFSET 0 means no offset */
        den = 1;
        ok = 1;
    }
    if (ok) {
        ratio_set(u, field, num, den);
    } else if (u->edit_buf[0] != '\0') {
        /* Garbage: keep the previous value rather than inventing one. */
        u->edit_buf[0] = '\0';
        return mark(u);
    }

    u->edit_field = EDIT_NONE;
    u->edit_buf[0] = '\0';
    if (advance && field + 1 < 32)
        u->edit_field = field + 1;
    return mark(u);
}

/* ------------------------------------------------------------------ drawing */

static void draw_text_trim(canvas_t *c, font_t *f, const char *s, float x, float y,
                           float px, float tracking, uint32_t color, float maxw)
{
    const char *p = s;
    while (*p && text_width(f, p, px, tracking) > maxw)
        p++;
    text_draw(c, f, p, x, y, px, tracking, color, ALIGN_LEFT);
}

static void draw_button(canvas_t *c, ui_t *u, box_t b, const char *label, float size,
                        uint32_t fill, uint32_t stroke, uint32_t text_col, int sel)
{
    d_rrect(c, b.x, b.y, b.w, b.h, 5.0f, fill);
    d_rrect_border(c, b.x, b.y, b.w, b.h, 5.0f, sel ? 2.0f : 1.0f, stroke);
    if (label && *label)
        text_draw(c, u->bold, label, b.x + b.w * 0.5f, b.y + b.h * 0.5f + size * 0.35f,
                  size, 0.6f, text_col, ALIGN_CENTER);
}

/* Ratio box: shows its value, or the text being typed plus a caret. */
static void draw_ratio_box(canvas_t *c, ui_t *u, box_t b, const char *value, int editing,
                           int sel, uint32_t fill, uint32_t stroke, uint32_t text_col,
                           uint32_t edit_col)
{
    const float size = 10.0f;
    const char *shown = editing ? u->edit_buf : value;

    d_rrect(c, b.x, b.y, b.w, b.h, 5.0f, fill);
    d_rrect_border(c, b.x, b.y, b.w, b.h, 5.0f, (editing || sel) ? 2.0f : 1.0f,
                   editing ? edit_col : stroke);

    float ty = b.y + b.h * 0.5f + size * 0.35f;
    float w = text_width(u->bold, shown, size, 0.6f);
    float cx = b.x + b.w * 0.5f - (editing ? 4.0f : 0.0f);
    if (w > 0.0f)
        text_draw(c, u->bold, shown, cx, ty, size, 0.6f, editing ? edit_col : text_col,
                  ALIGN_CENTER);
    if (editing) {
        float x = cx + w * 0.5f + 2.0f;
        d_rect(c, x, ty - size * 0.85f, 1.5f, size * 1.1f, COL_ACCENT);
    }
}

/* One piece of a cell in radians, with the usual slot of background at both
 * ends so neighbouring steps stay readable. */
static void draw_cell_arc(canvas_t *c, float cx, float cy, float rm, float th, int t0, int t1,
                          int loop, uint32_t col, int pad_lo, int pad_hi)
{
    float a0 = (float)t0 / (float)loop * TAU;
    float a1 = (float)t1 / (float)loop * TAU;
    if (a1 <= a0)
        return;
    float pad = (a1 - a0) * 0.14f;
    if (pad_lo)
        a0 += pad;
    if (pad_hi)
        a1 -= pad;
    if (a1 > a0)
        d_arc(c, cx, cy, rm - th, rm + th, a0, a1, col);
}

static void draw_rings(ui_t *u, canvas_t *c)
{
    float cx = u->rcx, cy = u->rcy;
    double pos = loop_pos(u);
    float loop = (float)loop_ticks(u);

    if (u->sel >= 0 && u->sel < 8) {
        float rm = u->r0 + ((float)u->sel + 0.5f) * u->ring_w;
        d_arc(c, cx, cy, rm - u->ring_w * 0.5f, rm + u->ring_w * 0.5f, 0.0f, TAU,
              COL_ACC_TINT);
    }

    /* Hovering a sample slot lights up that track's ring. */
    if (u->hover >= 0 && u->hover < 8) {
        float rm = u->r0 + ((float)u->hover + 0.5f) * u->ring_w;
        d_arc(c, cx, cy, rm - u->ring_w * 0.5f, rm + u->ring_w * 0.5f, 0.0f, TAU,
              COL_ACC_HOVER);
    }

    for (int k = 0; k <= 8; k++)
        d_circle_border(c, cx, cy, u->r0 + (float)k * u->ring_w, 1.0f, COL_RING);
    d_circle_border(c, cx, cy, 7.0f, 1.0f, COL_RING);

    /* Bar markers: a long bright tick where each bar starts, with a short dim
     * tick on the beats in between, so the loop reads as bars and beats even
     * when no track is loaded. */
    {
        int loop = loop_ticks(u);
        for (int t = 0; t < loop; t += TICKS_PER_BEAT) {
            int bar = (t % TICKS_PER_BAR) == 0;
            float a = (float)t / (float)loop * TAU;
            float ix = sinf(a), iy = -cosf(a);
            float ri = u->r_out + 4.0f;
            float ro = u->r_out + (bar ? 12.0f : 8.0f);
            d_line(c, cx + ix * ri, cy + iy * ri, cx + ix * ro, cy + iy * ro,
                   bar ? 2.0f : 1.2f, bar ? COL_TEXT : COL_DIM);
        }
    }

    if (u->hover >= 0 && u->hover < 8) {
        float rm = u->r0 + ((float)u->hover + 0.5f) * u->ring_w;
        d_circle_border(c, cx, cy, rm - u->ring_w * 0.5f, 1.4f, COL_ACC_SOFT);
        d_circle_border(c, cx, cy, rm + u->ring_w * 0.5f, 1.4f, COL_ACC_SOFT);
    }

    for (int v = 0; v < 8; v++) {
        int cc = cell_count(u, v);
        int dt = div_ticks(u, v);
        int kt = kill_ticks(u, v);
        int air = track_loaded(u, v);

        float rm = u->r0 + ((float)v + 0.5f) * u->ring_w;
        float th = u->ring_w * 0.33f;
        int cur = current_cell(u, v);
        int muted = u->mute[v];

        /* Kill hits, marked on the outside edge of the band. */
        int kills = (air && !muted && kt > 0) ? loop_ticks(u) / kt : 0;
        int kp = kill_off_ticks(u, v);
        for (int k = 0; k < kills; k++) {
            int tk = (int)(((long)k * (long)kt + (long)kp) % (long)loop_ticks(u));
            float a = ((float)tk / loop) * TAU;
            float ix = sinf(a), iy = -cosf(a);
            d_line(c, cx + ix * (rm + th - 1.0f), cy + iy * (rm + th - 1.0f),
                   cx + ix * (rm + th + 3.0f), cy + iy * (rm + th + 3.0f), 1.4f,
                   COL_KILL_DIM);
        }

        for (int s = 0; air && s < cc; s++) {
            int st = cell_start(u, v, s);
            int en = st + dt;
            int killed = cell_killed(u, v, s);
            uint32_t col = (killed || muted) ? COL_KILLED : COL_ACCENT;
            if (!killed && !muted && s == cur)
                col = COL_ACC_HI;
            int lp = loop_ticks(u);
            if (en <= lp) {
                draw_cell_arc(c, cx, cy, rm, th, st, en, lp, col, 1, 1);
            } else {
                /* The step runs past the loop point and carries on at its start.
                 * The two halves meet there, so the join gets no slot. */
                draw_cell_arc(c, cx, cy, rm, th, st, lp, lp, col, 1, 0);
                draw_cell_arc(c, cx, cy, rm, th, 0, en - lp, lp, col, 0, 1);
            }
        }

        if (air && !muted) { /* an empty or muted track has no playhead */
            float a = (float)(pos / (double)loop_ticks(u)) * TAU;
            float ix = sinf(a), iy = -cosf(a);
            d_line(c, cx + ix * (rm - th - 3.0f), cy + iy * (rm - th - 3.0f),
                   cx + ix * (rm + th + 3.0f), cy + iy * (rm + th + 3.0f), 1.6f, COL_TICK);
        }
    }
}

static void draw_rails(ui_t *u, canvas_t *c)
{
    text_draw(c, u->bold, "DIV", u->div_btn[0].x + u->div_btn[0].w * 0.5f, 22.0f, 10.0f,
              2.6f, COL_DIM, ALIGN_CENTER);
    text_draw(c, u->bold, "KILL", u->kill_btn[0].x + u->kill_btn[0].w * 0.5f, 22.0f,
              10.0f, 2.6f, COL_DIM, ALIGN_CENTER);
    text_draw(c, u->bold, "OFFSET", u->off_btn[0].x + u->off_btn[0].w * 0.5f, 22.0f,
              9.5f, 2.0f, COL_DIM, ALIGN_CENTER);
    text_draw(c, u->bold, "FULL", u->full_btn[0].x + u->full_btn[0].w * 0.5f, 22.0f,
              10.0f, 2.6f, COL_KILL, ALIGN_CENTER);
    text_draw(c, u->bold, "OFFSET", u->koff_btn[0].x + u->koff_btn[0].w * 0.5f, 22.0f,
              9.5f, 2.0f, COL_DIM, ALIGN_CENTER);
    text_draw(c, u->bold, "MUTE", u->mute_btn[0].x + u->mute_btn[0].w * 0.5f, 22.0f,
              10.0f, 2.6f, COL_DIM, ALIGN_CENTER);

    for (int i = 0; i < 8; i++) {
        int sel = (i == u->sel);
        int air = track_loaded(u, i);
        int killing = track_has_kills(u, i);
        uint32_t fill = air ? COL_BTN : COL_PANEL;
        uint32_t line = sel ? COL_ACCENT : (air ? COL_STROKE : COL_FAINT);
        char buf[16];

        format_ratio(u->div_num[i], u->div_den[i], buf, sizeof buf);
        draw_ratio_box(c, u, u->div_btn[i], buf, u->edit_field == EDIT_DIV(i), sel, fill,
                       line, air ? (sel ? COL_ACCENT : COL_DIM) : COL_FAINT, COL_ACCENT);

        format_num_or_zero(u->kill_num[i], u->kill_den[i], buf, sizeof buf);
        draw_ratio_box(c, u, u->kill_btn[i], buf, u->edit_field == EDIT_KILL(i), sel,
                       killing ? CDM_RGBA(40, 26, 25, 255) : fill,
                       sel ? COL_ACCENT : (killing ? COL_KILL : line),
                       killing ? COL_KILL : (air ? COL_DIM : COL_FAINT), COL_ACCENT);

        format_offset(u, i, buf, sizeof buf);
        draw_ratio_box(c, u, u->off_btn[i], buf, u->edit_field == EDIT_OFF(i), sel, fill,
                       line, air ? (sel ? COL_ACCENT : COL_TEXT) : COL_FAINT, COL_ACCENT);

        {
            box_t fb = u->full_btn[i];
            int on = u->full[i] && air;
            d_rrect(c, fb.x, fb.y, fb.w, fb.h, 5.0f, on ? COL_KILL : fill);
            d_rrect_border(c, fb.x, fb.y, fb.w, fb.h, 5.0f, on ? 1.6f : 1.0f,
                           on ? COL_KILL : line);
        }

        format_ratio(u->koff_num[i], u->koff_den[i], buf, sizeof buf);
        draw_ratio_box(c, u, u->koff_btn[i], buf, u->edit_field == EDIT_KOFF(i), sel, fill,
                       line, air ? (sel ? COL_ACCENT : COL_DIM) : COL_FAINT, COL_ACCENT);

        {
            box_t mb = u->mute_btn[i];
            int on = u->mute[i];
            d_rrect(c, mb.x, mb.y, mb.w, mb.h, 5.0f, on ? CDM_RGBA(40, 26, 25, 255) : fill);
            d_rrect_border(c, mb.x, mb.y, mb.w, mb.h, 5.0f, on ? 1.6f : 1.0f,
                           on ? COL_KILL : line);
            text_draw(c, u->bold, "M", mb.x + mb.w * 0.5f, mb.y + mb.h * 0.5f + 4.0f, 11.0f,
                      0.6f, on ? COL_KILL : (air ? COL_DIM : COL_FAINT), ALIGN_CENTER);
        }
    }
}

static int transport_running(const ui_t *u)
{
    if (u->host_attached && u->host_playing)
        return 1;
    return u->playing;
}

/* The host owns the clock while it rolls, so the button must not fight it. */
static int transport_locked(const ui_t *u)
{
    return u->host_attached && u->host_playing;
}

static int toggle_transport(ui_t *u)
{
    if (transport_locked(u))
        return 0;
    u->playing = !u->playing;
    for (int v = 0; v < 8; v++)
        u->prev_cell[v] = current_cell(u, v);
    if (u->mix)
        mixer_play(u->mix, u->playing);
    return mark(u);
}

static void draw_transport(ui_t *u, canvas_t *c)
{
    box_t b = u->play_box;
    int running = transport_running(u);
    int locked = transport_locked(u);
    float mx = b.x + b.w * 0.5f, my = b.y + b.h * 0.5f;

    uint32_t stroke = locked ? COL_FAINT : (running ? COL_ACCENT : COL_STROKE);
    uint32_t icon = locked ? COL_FAINT : (running ? COL_ACCENT : COL_TEXT);

    d_rrect(c, b.x, b.y, b.w, b.h, 5.0f, (running && !locked) ? COL_BTN_SEL : COL_BTN);
    d_rrect_border(c, b.x, b.y, b.w, b.h, 5.0f, running ? 1.6f : 1.0f, stroke);

    if (running) {
        d_rrect(c, mx - 8.0f, my - 10.0f, 5.0f, 20.0f, 1.5f, icon);
        d_rrect(c, mx + 3.0f, my - 10.0f, 5.0f, 20.0f, 1.5f, icon);
    } else {
        d_triangle(c, mx - 6.0f, my - 10.0f, mx - 6.0f, my + 10.0f, mx + 11.0f, my, icon);
    }
}

static void draw_bottom(ui_t *u, canvas_t *c)
{
    char buf[16];

    d_line(c, 20.0f, 282.0f, CANVAS_W - 20.0f, 282.0f, 1.0f, COL_RULE);
    d_line(c, 92.0f, 292.0f, 92.0f, 492.0f, 1.0f, COL_RULE);
    d_line(c, 764.0f, 292.0f, 764.0f, 492.0f, 1.0f, COL_RULE);

    text_draw(c, u->reg, "LENGTH", 20.0f, 296.0f, 9.0f, 1.4f, COL_DIM, ALIGN_LEFT);
    snprintf(buf, sizeof buf, "%d", u->bars);
    d_rrect(c, u->len_box.x, u->len_box.y, u->len_box.w, u->len_box.h, 5.0f, COL_BTN);
    d_rrect_border(c, u->len_box.x, u->len_box.y, u->len_box.w, u->len_box.h, 5.0f, 1.0f,
                   COL_STROKE);
    text_draw(c, u->bold, buf, u->len_box.x + 18.0f,
              u->len_box.y + u->len_box.h * 0.5f + 4.5f, 13.0f, 0.4f, COL_TEXT,
              ALIGN_CENTER);
    text_draw(c, u->reg, "bars", u->len_box.x + 45.0f,
              u->len_box.y + u->len_box.h * 0.5f + 2.8f, 8.0f, 0.2f, COL_DIM,
              ALIGN_CENTER);

    text_draw(c, u->reg, "BPM", 20.0f, 344.0f, 9.0f, 1.4f, COL_DIM, ALIGN_LEFT);
    snprintf(buf, sizeof buf, "%d", (int)(u->bpm + 0.5f));
    d_rrect(c, u->bpm_box.x, u->bpm_box.y, u->bpm_box.w, u->bpm_box.h, 5.0f, COL_BTN);
    d_rrect_border(c, u->bpm_box.x, u->bpm_box.y, u->bpm_box.w, u->bpm_box.h, 5.0f,
                   u->dragging ? 2.0f : 1.0f,
                   u->dragging ? COL_ACCENT : COL_STROKE);
    text_draw(c, u->bold, buf, u->bpm_box.x + u->bpm_box.w * 0.5f,
              u->bpm_box.y + u->bpm_box.h * 0.5f + 5.5f, 17.0f, 0.8f, COL_TEXT,
              ALIGN_CENTER);

    for (int i = 0; i < 8; i++) {
        int sel = (i == u->sel);
        int hov = (i == u->hover);
        int loaded = (u->smp_label[i][0] != '\0');
        uint32_t fill = !loaded ? COL_PANEL : ((hov || sel) ? COL_BTN_SEL : COL_BTN);
        uint32_t stroke = hov ? COL_ACCENT : (sel ? COL_ACCENT : (loaded ? COL_STROKE : COL_FAINT));
        uint32_t text = loaded ? COL_TEXT : (sel || hov ? COL_ACCENT : COL_DIM);

        draw_button(c, u, u->slot_btn[i], loaded ? u->smp_label[i] : "[LOAD]",
                    loaded ? 10.5f : 11.0f, fill, stroke, text, sel || hov);
    }

    for (int i = 0; i < 8; i++) {
        float x = u->pad_x[i], y = u->pad_y[i];
        float f = u->trig[i];
        int air = track_loaded(u, i);
        int live = air && !u->mute[i]; /* a muted pad reads like an empty one */
        char db[8];

        d_circle(c, x, y, PAD_R, air ? COL_BTN : COL_PANEL);
        if (f > 0.0f && live) {
            int a = (int)(30.0f + 200.0f * f);
            d_circle(c, x, y, PAD_R - 2.0f, CDM_RGBA(245, 167, 66, a));
        }
        d_circle_border(c, x, y, PAD_R, i == u->sel ? 1.6f : 1.0f,
                        i == u->sel ? COL_ACCENT : (air ? COL_STROKE : COL_FAINT));

        /* Level ring: the travel swept clockwise from the bottom of the pad. */
        if (u->vol[i] > VOL_OFF_POS)
            draw_vol_ring(c, x, y, u->vol[i], live ? COL_ACCENT : COL_FAINT);

        format_vol(u->vol[i], db, sizeof db);
        text_draw(c, u->bold, db, x, y + 2.8f, 7.5f, 0.3f, live ? COL_TEXT : COL_FAINT,
                  ALIGN_CENTER);
    }

    draw_transport(u, c);

    /* The name runs down the right margin, centred in its column. */
    text_draw_vertical(c, u->bold, "SPYRO24", 805.0f, 391.0f, 23.0f, 9.0f, COL_BRAND);
}

static void draw_browser(ui_t *u, canvas_t *c)
{
    box_t b = browser_box();
    box_t lb = browser_list_box();

    d_rect(c, 0.0f, 0.0f, CANVAS_W, CANVAS_H, COL_SHADE);

    d_rrect(c, b.x, b.y, b.w, b.h, 8.0f, COL_PANEL);
    d_rrect_border(c, b.x, b.y, b.w, b.h, 8.0f, 1.0f, COL_STROKE);

    char hdr[48];
    snprintf(hdr, sizeof hdr, "LOAD SAMPLE  -  TRACK %d", u->browser_track + 1);
    text_draw(c, u->bold, hdr, b.x + 20.0f, b.y + 34.0f, 12.0f, 2.0f, COL_TEXT,
              ALIGN_LEFT);
    text_draw(c, u->bold, "SHOWING WAV / MP3", b.x + b.w - 20.0f, b.y + 34.0f,
              10.0f, 2.0f, COL_FAINT, ALIGN_RIGHT);

    d_rrect(c, b.x + 16.0f, b.y + 48.0f, b.w - 32.0f, 34.0f, 4.0f, COL_BTN);
    draw_text_trim(c, u->reg, u->browser_dir, b.x + 26.0f, b.y + 70.0f, 11.0f, 0.4f,
                   COL_DIM, b.w - 52.0f);

    d_rrect(c, lb.x, lb.y, lb.w, lb.h, 4.0f, COL_BG);
    d_rrect_border(c, lb.x, lb.y, lb.w, lb.h, 4.0f, 1.0f, COL_FAINT);

    for (int row = 0; row < LIST_ROWS; row++) {
        int idx = u->browser_scroll + row;
        if (idx >= u->entry_n)
            break;

        float ry = lb.y + (float)row * LIST_ROW_H;
        if (idx == u->browser_hover)
            d_rrect(c, lb.x + 2.0f, ry + 1.0f, lb.w - 4.0f, LIST_ROW_H - 2.0f, 3.0f,
                    COL_ROW);

        char label[208];
        if (u->entry[idx].is_dir)
            snprintf(label, sizeof label, "%s/", u->entry[idx].name);
        else
            snprintf(label, sizeof label, "%s", u->entry[idx].name);

        uint32_t col = u->entry[idx].is_dir ? COL_FOLDER : COL_TEXT;
        draw_text_trim(c, u->reg, label, lb.x + 12.0f, ry + LIST_ROW_H * 0.5f + 4.0f,
                       11.5f, 0.4f, col, lb.w - 24.0f);
    }

    if (u->entry_n > LIST_ROWS) {
        float tx = lb.x + lb.w + 8.0f;
        float frac = (float)LIST_ROWS / (float)u->entry_n;
        float thumb = lb.h * frac;
        int maxs = u->entry_n - LIST_ROWS;
        float t = maxs > 0 ? (float)u->browser_scroll / (float)maxs : 0.0f;
        d_rrect(c, tx, lb.y, 4.0f, lb.h, 2.0f, COL_BTN);
        d_rrect(c, tx, lb.y + (lb.h - thumb) * t, 4.0f, thumb, 2.0f, COL_STROKE);
    }

    text_draw(c, u->reg, "click a file to load it into this track   -   esc to cancel",
              b.x + 20.0f, b.y + b.h - 20.0f, 10.0f, 0.4f, COL_FAINT, ALIGN_LEFT);

    if (u->smp_label[u->browser_track][0]) {
        char cur[64];
        snprintf(cur, sizeof cur, "current: %s", u->smp_label[u->browser_track]);
        text_draw(c, u->reg, cur, b.x + b.w - 20.0f, b.y + b.h - 20.0f, 10.0f, 0.4f,
                  COL_DIM, ALIGN_RIGHT);
    }
}

void ui_frame(ui_t *u, canvas_t *c)
{
    layout(u);
    canvas_clear(c, COL_BG);
    d_rect(c, 0.0f, 282.0f, CANVAS_W, CANVAS_H - 282.0f, COL_BAND);

    draw_rings(u, c);
    draw_rails(u, c);
    draw_bottom(u, c);
    if (u->browser_open)
        draw_browser(u, c);

    u->dirty = 0;
}

/* -------------------------------------------------------------- transport */

/* Hand the engine a snapshot of everything it needs to make sound. */
static void push_kit(ui_t *u)
{
    if (!u->mix)
        return;
    audio_kit k;
    memset(&k, 0, sizeof k);
    k.bpm = (double)u->bpm;
    k.loop_ticks = loop_ticks(u);
    k.ticks_per_beat = TICKS_PER_BEAT;
    for (int v = 0; v < 8; v++) {
        k.tr[v].div_ticks = div_ticks(u, v);
        k.tr[v].kill_ticks = kill_ticks(u, v);
        k.tr[v].off_ticks = phase_ticks(u, v);
        k.tr[v].kill_off_ticks = kill_off_ticks(u, v);
        k.tr[v].full = u->full[v];
        k.tr[v].gain = u->mute[v] ? 0.0f : vol_gain(u->vol[v]); /* mute is a gain of 0 */
    }
    mixer_set_kit(u->mix, &k);
}

void ui_tick(ui_t *u, float dt)
{
    if (u->kit_dirty) {
        push_kit(u);
        u->kit_dirty = 0;
    }

    if (u->mix && mixer_playing(u->mix))
        u->beats = mixer_pos_ticks(u->mix) / (double)TICKS_PER_BEAT;
    else if (transport_locked(u))
        u->beats = u->host_beats;
    else if (u->playing)
        u->beats += (double)dt * (double)u->bpm / 60.0;

    for (int v = 0; v < 8; v++) {
        int cs = current_cell(u, v);
        if (cs != u->prev_cell[v]) {
            u->prev_cell[v] = cs;
            if (u->playing && cs >= 0 && !cell_killed(u, v, cs))
                u->trig[v] = 1.0f;
        }
        if (u->trig[v] > 0.0f) {
            u->trig[v] -= dt * 3.5f;
            if (u->trig[v] < 0.0f)
                u->trig[v] = 0.0f;
        }
    }
}

int ui_animating(const ui_t *u)
{
    if (transport_running(u))
        return 1;
    for (int v = 0; v < 8; v++)
        if (u->trig[v] > 0.0f)
            return 1;
    return 0;
}

int ui_needs_frame(const ui_t *u)
{
    return u->dirty || ui_animating(u);
}

/* -------------------------------------------------------------------- input */

ui_t *ui_create(void)
{
    static const char *bold_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "C:/Windows/Fonts/segoeuib.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
        NULL,
    };
    static const char *reg_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        NULL,
    };

    ui_t *u = calloc(1, sizeof *u);
    if (!u)
        return NULL;

    for (int i = 0; bold_paths[i] && !u->bold; i++)
        u->bold = font_load(bold_paths[i]);
    for (int i = 0; reg_paths[i] && !u->reg; i++)
        u->reg = font_load(reg_paths[i]);

    if (!u->bold || !u->reg) {
        fprintf(stderr, "ui: no usable font found\n");
        font_free(u->bold);
        font_free(u->reg);
        free(u);
        return NULL;
    }

    u->bars = 2;
    u->bpm = 120.0f;
    u->playing = 0;
    u->sel = 0;
    u->hover = -1;
    u->vol_drag = -1;
    u->browser_hover = -1;
    u->edit_field = EDIT_NONE;
    u->dirty = 1;

    for (int v = 0; v < 8; v++) {
        u->div_num[v] = 1;
        u->div_den[v] = (v < 2) ? 4 : 8;
        u->kill_num[v] = 0; /* the kill starts switched off */
        u->kill_den[v] = 1;
    }

    for (int v = 0; v < 8; v++) {
        u->off_num[v] = 0;
        u->off_den[v] = 1;
        u->koff_num[v] = 0; /* the kill grid starts on tick 0 */
        u->koff_den[v] = 1;
        u->full[v] = 1;
        u->vol[v] = vol_pos_of_db(0.0f); /* every track starts at 0 dB */
        u->prev_cell[v] = 0;
    }
    return u;
}

void ui_destroy(ui_t *u)
{
    if (!u)
        return;
    font_free(u->bold);
    font_free(u->reg);
    free(u);
}

static int cycle_bars(ui_t *u)
{
    int n = (int)(sizeof BARS_VALUES / sizeof BARS_VALUES[0]);
    int idx = 0;
    for (int i = 0; i < n; i++)
        if (BARS_VALUES[i] == u->bars)
            idx = i;
    u->bars = BARS_VALUES[(idx + 1) % n];
    for (int v = 0; v < 8; v++)
        u->prev_cell[v] = current_cell(u, v);
    return mark(u);
}

/* Wheel over a ratio box steps through the common note values. */
/* Offset presets: none first, then common note values. */
static const int OFF_NUM[6] = { 0, 1, 1, 1, 1, 1 };
static const int OFF_DEN[6] = { 1, 32, 16, 8, 4, 2 };

static int cycle_offset(ui_t *u, int field, int dir)
{
    int n = (int)(sizeof OFF_DEN / sizeof OFF_DEN[0]);
    int num, den, idx = 0;
    ratio_of(u, field, &num, &den);
    for (int k = 1; k < n; k++)
        if (OFF_NUM[k] == num && OFF_DEN[k] == den)
            idx = k;
    idx = (idx + (dir > 0 ? n - 1 : 1)) % n;
    ratio_set(u, field, OFF_NUM[idx], OFF_DEN[idx]);
    return mark(u);
}

/* Kill presets: 0 (off) first, then the note values. */
static const int KILL_NUM[8] = { 0, 1, 1, 1, 1, 1, 1, 1 };
static const int KILL_DEN[8] = { 1, 1, 2, 3, 4, 6, 8, 16 };

static int cycle_kill(ui_t *u, int i, int dir)
{
    int n = (int)(sizeof KILL_DEN / sizeof KILL_DEN[0]);
    int idx = 0;
    for (int k = 1; k < n; k++)
        if (KILL_NUM[k] == u->kill_num[i] && KILL_DEN[k] == u->kill_den[i])
            idx = k;
    idx = (idx + (dir > 0 ? n - 1 : 1)) % n;
    u->kill_num[i] = KILL_NUM[idx];
    u->kill_den[i] = KILL_DEN[idx];
    u->prev_cell[i] = current_cell(u, i);
    return mark(u);
}

static int cycle_ratio(ui_t *u, int field, int dir)
{
    int num, den;
    ratio_of(u, field, &num, &den);
    int n = (int)(sizeof STD_DEN / sizeof STD_DEN[0]);
    int idx = 0;
    for (int i = 0; i < n; i++)
        if (STD_DEN[i] == den)
            idx = i;
    idx = (idx + (dir > 0 ? n - 1 : 1)) % n;
    ratio_set(u, field, STD_NUM[idx], STD_DEN[idx]);
    return mark(u);
}

int ui_pointer(ui_t *u, int event, float x, float y, int button)
{
    (void)button;
    layout(u);

    if (u->browser_open) {
        box_t panel = browser_box();
        box_t lb = browser_list_box();

        if (event == UI_DOWN) {
            if (!in_box(panel, x, y)) {
                u->browser_open = 0;
                return mark(u);
            }
            if (in_box(lb, x, y)) {
                int row = (int)((y - lb.y) / LIST_ROW_H);
                browser_choose(u, u->browser_scroll + row);
                return mark(u);
            }
            return 0;
        }
        if (event == UI_MOVE) {
            int h = -1;
            if (in_box(lb, x, y)) {
                int row = (int)((y - lb.y) / LIST_ROW_H);
                int idx = u->browser_scroll + row;
                if (idx >= 0 && idx < u->entry_n)
                    h = idx;
            }
            if (h != u->browser_hover) {
                u->browser_hover = h;
                return mark(u);
            }
        }
        return 0;
    }

    if (event == UI_UP) {
        if (u->dragging || u->vol_drag >= 0) {
            u->dragging = 0;
            u->vol_drag = -1;
            return mark(u);
        }
        return 0;
    }

    if (event == UI_MOVE) {
        if (u->dragging) {
            u->bpm = clampf(u->drag_bpm + (u->drag_y0 - y) * 0.5f, 40.0f, 240.0f);
            return mark(u);
        }
        if (u->vol_drag >= 0) {
            u->vol[u->vol_drag] = clampf(u->vol_p0 + (u->vol_y0 - y) * 0.007f, 0.0f, 1.0f);
            return mark(u);
        }
        int h = -1;
        for (int i = 0; i < 8; i++)
            if (in_box(u->slot_btn[i], x, y))
                h = i;
        if (h != u->hover) {
            u->hover = h;
            return mark(u);
        }
        return 0;
    }

    /* UI_DOWN */
    int changed = 0;
    int was_editing = (u->edit_field != EDIT_NONE);

    for (int i = 0; i < 8; i++) {
        if (in_box(u->div_btn[i], x, y)) {
            if (u->edit_field != EDIT_DIV(i))
                changed |= edit_commit(u, 0);
            u->sel = i;
            changed |= edit_start(u, EDIT_DIV(i));
            return changed | mark(u);
        }
        if (in_box(u->kill_btn[i], x, y)) {
            if (u->edit_field != EDIT_KILL(i))
                changed |= edit_commit(u, 0);
            u->sel = i;
            changed |= edit_start(u, EDIT_KILL(i));
            return changed | mark(u);
        }
        if (in_box(u->off_btn[i], x, y)) {
            if (u->edit_field != EDIT_OFF(i))
                changed |= edit_commit(u, 0);
            u->sel = i;
            changed |= edit_start(u, EDIT_OFF(i));
            return changed | mark(u);
        }
        if (in_box(u->full_btn[i], x, y)) {
            u->full[i] = !u->full[i];
            u->sel = i;
            return mark(u);
        }
        if (in_box(u->koff_btn[i], x, y)) {
            if (u->edit_field != EDIT_KOFF(i))
                changed |= edit_commit(u, 0);
            u->sel = i;
            changed |= edit_start(u, EDIT_KOFF(i));
            return changed | mark(u);
        }
        if (in_box(u->mute_btn[i], x, y)) {
            u->mute[i] = !u->mute[i];
            u->sel = i;
            return mark(u);
        }
        if (in_box(u->slot_btn[i], x, y)) {
            u->sel = i;
            browser_open_for(u, i);
            return mark(u);
        }
        float dx = x - u->pad_x[i], dy = y - u->pad_y[i];
        if (dx * dx + dy * dy <= PAD_R * PAD_R) {
            u->sel = i;
            u->vol_drag = i;
            u->vol_y0 = y;
            u->vol_p0 = u->vol[i];
            return mark(u);
        }
    }

    /* Any other click finishes an edit. */
    if (was_editing)
        changed |= edit_commit(u, 0);

    if (in_box(u->len_box, x, y))
        return changed | cycle_bars(u);

    if (in_box(u->bpm_box, x, y)) {
        u->dragging = 1;
        u->drag_y0 = y;
        u->drag_bpm = u->bpm;
        return changed | mark(u);
    }

    if (in_box(u->play_box, x, y))
        return changed | toggle_transport(u);

    /* The rings display the DIV/KILL grid, so a click only picks the track. */
    float dx = x - u->rcx, dy = y - u->rcy;
    float len = sqrtf(dx * dx + dy * dy);
    if (len >= u->r0 && len <= u->r_out) {
        int v = (int)((len - u->r0) / u->ring_w);
        if (v > 7)
            v = 7;
        if (v < 0)
            v = 0;
        if (u->sel != v) {
            u->sel = v;
            return mark(u);
        }
        return changed;
    }
    return changed;
}

int ui_wheel(ui_t *u, float dy, float x, float y)
{
    layout(u);

    if (u->browser_open) {
        int maxs = u->entry_n - LIST_ROWS;
        if (maxs < 0)
            maxs = 0;
        int s = u->browser_scroll - (int)(dy > 0 ? 1 : (dy < 0 ? -1 : 0));
        if (s < 0)
            s = 0;
        if (s > maxs)
            s = maxs;
        if (s != u->browser_scroll) {
            u->browser_scroll = s;
            return mark(u);
        }
        return 0;
    }

    if (in_box(u->bpm_box, x, y)) {
        u->bpm = clampf(u->bpm + dy * 2.0f, 40.0f, 240.0f);
        return mark(u);
    }
    if (in_box(u->len_box, x, y))
        return cycle_bars(u);

    for (int i = 0; i < 8; i++) {
        if (in_box(u->div_btn[i], x, y))
            return cycle_ratio(u, EDIT_DIV(i), (int)dy);
        if (in_box(u->kill_btn[i], x, y))
            return cycle_kill(u, i, (int)dy);
        if (in_box(u->off_btn[i], x, y))
            return cycle_offset(u, EDIT_OFF(i), (int)dy);
        if (in_box(u->koff_btn[i], x, y))
            return cycle_offset(u, EDIT_KOFF(i), (int)dy);
    }

    for (int i = 0; i < 8; i++) {
        float dx = x - u->pad_x[i], dy2 = y - u->pad_y[i];
        if (dx * dx + dy2 * dy2 <= PAD_R * PAD_R) {
            /* One notch is 2 dB, which is what a fader feels like. */
            float db = vol_db(u->vol[i]) + dy * 2.0f;
            u->vol[i] = vol_pos_of_db(clampf(db, VOL_MIN_DB, VOL_MAX_DB));
            u->sel = i;
            return mark(u);
        }
    }
    return 0;
}

int ui_key(ui_t *u, int key, int down)
{
    if (!down)
        return 0;

    if (u->browser_open) {
        if (key == 27) {
            u->browser_open = 0;
            return mark(u);
        }
        return 0;
    }

    if (u->edit_field != EDIT_NONE) {
        if (key == 27)
            return edit_cancel(u);
        if (key == 8) {
            size_t n = strlen(u->edit_buf);
            if (n > 0) {
                u->edit_buf[n - 1] = '\0';
                return mark(u);
            }
            return 0;
        }
        if (key == 13 || key == 9)
            return edit_commit(u, 1);
        return 0; /* swallow transport and other shortcuts while typing */
    }

    if (key == ' ')
        return toggle_transport(u);

    return 0;
}

int ui_text(ui_t *u, const char *text)
{
    if (u->edit_field == EDIT_NONE || !text)
        return 0;

    int changed = 0;
    for (const char *p = text; *p; p++) {
        if ((*p >= '0' && *p <= '9') || *p == '/') {
            size_t n = strlen(u->edit_buf);
            if (n + 1 < sizeof u->edit_buf) {
                u->edit_buf[n] = *p;
                u->edit_buf[n + 1] = '\0';
                changed = 1;
            }
        }
    }
    return changed ? mark(u) : 0;
}

int ui_drop(ui_t *u, float x, float y, const char *path)
{
    if (!path || !*path || !is_audio_file(path))
        return 0;

    layout(u);
    for (int i = 0; i < 8; i++) {
        if (!in_box(u->slot_btn[i], x, y))
            continue;
        path_dir(path, u->browser_dir, sizeof u->browser_dir);
        snprintf(u->smp_path[i], sizeof u->smp_path[i], "%s", path);
        sample_label(path, u->smp_label[i], sizeof u->smp_label[i]);
        if (u->mix)
            mixer_load(u->mix, i, path);
        u->sel = i;
        u->browser_open = 0;
        return mark(u);
    }
    return 0;
}

void ui_set_mixer(ui_t *u, mixer_t *m)
{
    u->mix = m;
    push_kit(u);
    u->kit_dirty = 0;
}

void ui_set_host_transport(ui_t *u, int playing, double beats)
{
    /* While the host rolls it owns the clock, so the internal one stays idle. */
    if (u->mix && playing)
        mixer_play(u->mix, 0);
    u->host_attached = 1;
    u->host_beats = beats;
    if (u->host_playing != playing) {
        u->host_playing = playing;
        u->dirty = 1;
    }
}

int ui_transport_running(const ui_t *u)
{
    return transport_running(u);
}
