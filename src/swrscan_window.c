/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - SWR scan: the antenna SWR over a span around the VFO, over every band at once, over one
 *  band or a segment of one's own
 *
 *  Ranges (the Range button): VFO, a span around the radio's TX frequency as before; All bands, one low-power pass
 *  over each amateur band the radio may transmit on, side by side, MFK picks one and a press looks closer; Band, one
 *  band whole; Segment, a part of a band named in BOARD_DATA/swr_segments.tsv ("name<TAB>low kHz<TAB>high kHz");
 *  Calibrate, the All bands pass on a 50 ohm dummy load: what the bridge reads there is taken off every reading
 *  after (BOARD_DATA/swr_cal.tsv, per band and power); a band reading over 1.5 has no load on it and is not kept.
 *  The bands are ham_bands.h's, the widest a licence has (40 m to 7.3 MHz, 80 m to 4.0): the radio goes on
 *  expeditions abroad, and the operator knows what the licence of the day covers. VFO, Band and Segment sweep over and over, the
 *  last sweep drawn faintly under the new one (an antenna trimmed shows at once); All bands and Calibrate once.
 *  Between two bands the carrier stops: the band's filter relays never switch under power. The beacons (NCDXF, the
 *  10 m and 6 m beacon segments) and the WSPR windows are stepped over: the carrier never sits on them.
 *  With Settings > TX Cal > TX outside ham bands on (the operator's licence covers it), the VFO range goes on to 5 and
 *  10 MHz across the bands: it says OUT OF BAND TX, unkeys between every two points outside the bands (the filters
 *  change anywhere there), and never sits on the distress, search and rescue, time signal and radio astronomy
 *  frequencies (safety[]).
 *  Power (Band, Segment, VFO): 1 W as before, or 2, 3, 5 W for a closer look; over 1 W, three readings over 3 in a
 *  row bring the power back to 1 W, and the sweep goes on there.
 *  The readings come on the radio thread: dialog_swrscan_update hands each to the GUI thread, where the sweep moves,
 *  stops between two bands and ends (the VFO settings put back there, as Stop does).
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "swrscan_window.h"

#include "gui_styles.h"
#include "theme.h"
#include "params/params.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "gui_events.h"
#include "key_input.h"
#include "screen_main.h"
#include "button_bar.h"
#include "msg.h"
#include "board.h"
#include "cfg/ham_bands.h"
#include "remote_server.h"
#include "scheduler.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define POINTS      50      /* across a span under 1 MHz */
#define POINTS_WIDE 100     /* from 1 MHz: one every 25 kHz at 2.5 MHz, so a narrow dip still shows (#107) */
#define POINTS_BAND 160     /* a band or a segment, looked at closely */
#define POINTS_MAX  POINTS_BAND
#define POINTS_ALL  32      /* each band in the All bands pass */
#define TOP_H       34      /* the frequencies above the grid */
#define BOTTOM_H    10
#define REKEY_MS    400     /* between two bands: unkeyed, the relays set, then the carrier again */
#define SETTLE_N    4       /* readings left out after the carrier comes: the first two or three read 1.0 to 2, rising */
#define SETTLE_ALIKE 6      /* then at most that many more, until two in a row agree within 0.15 */
#define GUARD_SWR   3.0f    /* over 1 W: three readings over it in a row bring the power back to 1 W */
#define GUARD_N     3
#define CAL_MAX_G   0.2f    /* the reflection a dummy load may show: SWR 1.5 */
#define SEGS_MAX    16
#define SEGS_PATH   BOARD_DATA "/swr_segments.tsv"
#define CAL_PATH    BOARD_DATA "/swr_cal.tsv"
#define SWEEPS_DIR  BOARD_DATA "/swr"           /* each sweep kept as CSV at Stop and at the end of a pass */

typedef enum { R_VFO, R_ALL, R_BAND, R_SEG, R_CAL, R_COUNT } range_t;

typedef struct {
    const char *name;
    int32_t     lo, hi;
} band_t;

/* The bands swept, edge to edge as ham_bands.h has them (bands_check reads them there at every opening): the widest
 * any licence has, for the expeditions abroad. Whether a licence covers all of it is the operator's to know */
static band_t bands[] = {
    {"160m", 1810000, 2000000},   {"80m", 3500000, 3800000},    {"60m", 5351500, 5366500},
    {"40m", 7000000, 7200000},    {"30m", 10100000, 10150000},  {"20m", 14000000, 14350000},
    {"17m", 18068000, 18168000},  {"15m", 21000000, 21450000},  {"12m", 24890000, 24990000},
    {"10m", 28000000, 29700000},  {"6m", 50000000, 52000000},
};
#define NB  (int)(sizeof(bands) / sizeof(bands[0]))

/* Where the carrier never sits: the NCDXF beacons, the WSPR windows (dial + 1400-1600 Hz), the beacon segments */
static const struct { int32_t lo, hi; } quiet[] = {
    {14099000, 14101000}, {18109000, 18111000}, {21149000, 21151000}, {24929000, 24931000}, {28199000, 28201000},
    {1838000, 1838300},   {3570000, 3570300},   {5366100, 5366400},   {7040000, 7040300},   {10140100, 10140400},
    {14097000, 14097300}, {18106000, 18106300}, {21096000, 21096300}, {24926000, 24926300}, {28126000, 28126300},
    {50294400, 50294700}, {28190000, 28225000}, {50400000, 50500000},
};

/* Where the carrier never sits either, the radio's "TX outside ham bands" on or not: the maritime distress and safety
 * frequencies (RR Appendix 15, the 2182 kHz guard band whole), the HF search and rescue on-scene ones (3023, 5680 kHz),
 * the standard frequency and time signal bands (RR 5.53-5.56) and the radio astronomy ones (13.36-13.41, 25.55-25.67
 * MHz) */
static const struct { int32_t lo, hi; } safety[] = {
    {2173500, 2190500},   {4122000, 4128000},   {4174500, 4180500},   {4204500, 4210500},   {6212000, 6218000},
    {6265000, 6271000},   {6309000, 6315000},   {8288000, 8294000},   {8373500, 8379500},   {8411500, 8417500},
    {12287000, 12293000}, {12517000, 12523000}, {12574000, 12580000}, {16417000, 16423000}, {16692000, 16698000},
    {16801500, 16807500}, {3020000, 3026000},   {5677000, 5683000},   {2495000, 2505000},   {4995000, 5005000},
    {9995000, 10005000},  {14990000, 15010000}, {19990000, 20010000}, {24990000, 25010000}, {13360000, 13410000},
    {25550000, 25670000},
};

static const float pwr_w[] = {1.0f, 2.0f, 3.0f, 5.0f};
#define NP  (int)(sizeof(pwr_w) / sizeof(pwr_w[0]))

/* Without swr_segments.tsv. No FT8 window among them: a segment sweeps over and over, its carrier back every pass */
static const struct { const char *name; int32_t lo, hi; } segs_default[] = {
    {"40m CW", 7000000, 7040000},    {"40m SSB", 7060000, 7200000},    {"20m CW", 14000000, 14070000},
    {"20m SSB", 14101000, 14350000}, {"15m SSB", 21151000, 21450000},  {"10m SSB", 28300000, 29000000},
};

static lv_obj_t *plot;
static float    swr[POINTS_MAX];        /* this sweep, NAN where not read (yet, or stepped over) */
static float    pwr[POINTS_MAX];        /* the power out at each reading, W: in the saved sweeps */
static float    ghost[POINTS_MAX];      /* the sweep before, drawn faintly */
static float    ghost_pwr[POINTS_MAX];
static bool     have_ghost;
static uint16_t points = POINTS;        /* in this sweep */
static uint16_t pos;
static int32_t  f_center, f_low, f_high;
static bool     scanning;
static bool     lin_scale;          /* else log */
static int32_t  span_hz;
static range_t  range;
static int      sel_band = 5;       /* 20m: the All bands cursor, the Band range's band */
static int      sel_seg;
static int      cur_band;           /* the band the All bands or Calibrate pass is on */
static int      pwr_i;              /* pwr_w[], back to 1 W at every opening */
static int      guard_n;
static int      settle_n;           /* readings still to leave out (SETTLE_N) */
static float    settle_v;           /* then the first point waits for two readings alike: the base's SWR lags its
                                     * power by a reading (2026-11-13, an EFHW: 1.0 at 0.5 W out, then 3.4) */
static int      settle_alike;       /* readings left to wait for them, 0 once the band's first point is read */
static bool     band_ok[NB];        /* the radio may transmit over all of it */
static float    ov[NB][POINTS_ALL]; /* the All bands pass, kept for the overview after a closer look */
static float    ov_pwr[NB][POINTS_ALL];
static bool     ov_done[NB];
static float    cal_g[NB][NP];      /* the bridge's reflection on the dummy load, taken off the readings */
static bool     cal_ok[NB][NP];
static lv_timer_t *rekey;
static atomic_bool step_due;        /* a reading on its way to the GUI thread: the radio's next ones wait */

static struct { char name[24]; int32_t lo, hi; } segs[SEGS_MAX];
static int      nsegs;

static ObserverDelayed *freq_obs, *linear_obs, *span_obs;

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void plot_key_cb(lv_event_t *e);
static void run_cb(button_data_t *b);
static void range_cb(button_data_t *b);
static void pick_cb(button_data_t *b);
static void power_cb(button_data_t *b);
static void scale_cb(button_data_t *b);
static const char *run_label();
static const char *range_label();
static const char *pick_label();
static const char *power_label();
static const char *scale_label();

static button_data_t btn_run   = { .type = BTN_TEXT_FN, .label_fn = run_label,      .press = run_cb };
static button_data_t btn_range = { .type = BTN_TEXT_FN, .label_fn = range_label,    .press = range_cb };
static button_data_t btn_pick  = { .type = BTN_TEXT_FN, .label_fn = pick_label,     .press = pick_cb };
static button_data_t btn_power = { .type = BTN_TEXT_FN, .label_fn = power_label,    .press = power_cb };
static button_data_t btn_scale = { .type = BTN_TEXT_FN, .label_fn = scale_label,    .press = scale_cb };

static buttons_page_t page = { { &btn_run, &btn_range, &btn_pick, &btn_power, &btn_scale } };

static dialog_t dialog = { .construct_cb = open_cb, .destruct_cb = close_cb, .btn_page = &page, .key_cb = plot_key_cb };

dialog_t *dialog_swrscan = &dialog;

static bool pass_range(void) {
    return range == R_ALL || range == R_CAL;
}

static int32_t freq_at(int p) {
    return f_low + (int64_t)(f_high - f_low) * p / (points - 1);
}

static bool is_quiet(int32_t f) {
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++)
        if (f >= quiet[i].lo && f <= quiet[i].hi) return true;
    for (size_t i = 0; i < sizeof(safety) / sizeof(safety[0]); i++)
        if (f >= safety[i].lo && f <= safety[i].hi) return true;
    return false;
}

/* Settings > TX Cal > TX outside ham bands: the VFO range may then go wider than a band, up to 10 MHz */
static bool outside_on(void) {
    return param_i_get(cfg_tx_outside_bands) != 0;
}

/* From point p on, the first the carrier may sit on; the ones stepped over stay NAN */
static int next_point(int p) {
    while (p < points && is_quiet(freq_at(p))) swr[p++] = NAN;
    return p;
}

static int band_of(int32_t f) {
    for (int i = 0; i < NB; i++)
        if (f >= bands[i].lo && f <= bands[i].hi) return i;
    return -1;
}

static int next_ok_band(int from, int dir) {
    for (int k = 1; k <= NB; k++) {
        int i = ((from + dir * k) % NB + NB) % NB;
        if (band_ok[i]) return i;
    }
    return -1;
}

/* The power's place in pwr_w[] */
static int pwr_idx(void) {
    return pwr_i;
}

/* The power the sweep asks */
static float scan_pwr(void) {
    return pwr_w[pwr_idx()];
}

/* The dummy load's reflection taken off: |G| - |G0|, back to SWR */
static float corrected(float v, int32_t f) {
    int b = band_of(f);

    if (range == R_CAL || b < 0 || !cal_ok[b][pwr_idx()] || !isfinite(v))
        return v;
    float g = (v - 1.0f) / (v + 1.0f) - cal_g[b][pwr_idx()];
    if (g < 0.0f) g = 0.0f;
    return (1.0f + g) / (1.0f - g);
}

static void set_band_sweep(int b) {
    cur_band = b;
    f_low = bands[b].lo;
    f_high = bands[b].hi;
    points = POINTS_ALL;
}

/* A flat line and the scan back at the low edge of the range. VFO: around the frequency the radio transmits on (the
 * other VFO in split, a TX shift's); the sweep stays where the radio transmits the same way (#107): a span past the
 * edge of HF (0.1 or 55 MHz), of a transverter's range or of the amateur band (with TX outside the amateur bands off,
 * #121) moves inside it, and one wider than the range takes all of it: 2.5 MHz on 40 m sweeps 7.0-7.3, the
 * frequencies on the plot say so. A point outside used to be refused by the radio, and the scan read the last
 * frequency again */
static void reset_scan() {
    int32_t lo, hi;

    f_center = radio_tx_freq();
    switch (range) {
        case R_VFO:
            f_low = f_center - span_hz / 2;
            f_high = f_center + span_hz / 2;
            if (cfg_tx_range(f_center, &lo, &hi)) {
                if (f_low < lo) {
                    f_high = LV_MIN(f_high + (lo - f_low), hi);
                    f_low = lo;
                } else if (f_high > hi) {
                    f_low = LV_MAX(f_low - (f_high - hi), lo);
                    f_high = hi;
                }
            }
            points = f_high - f_low >= 5000000 ? POINTS_MAX : f_high - f_low >= 1000000 ? POINTS_WIDE : POINTS;
            break;
        case R_BAND:
            f_low = bands[sel_band].lo;
            f_high = bands[sel_band].hi;
            points = POINTS_BAND;
            break;
        case R_SEG:
            f_low = segs[sel_seg].lo;
            f_high = segs[sel_seg].hi;
            points = POINTS_BAND;
            break;
        default:
            set_band_sweep(next_ok_band(-1, 1) < 0 ? 0 : next_ok_band(-1, 1));
            break;
    }
    for (int i = 0; i < POINTS_MAX; i++) swr[i] = NAN;
    have_ghost = false;
    guard_n = 0;
    pos = next_point(0);
}

/* Which bands the radio may transmit over, edge to edge (#121: a licence's setting, a transverter's range) */
static void bands_check(void) {
    for (int i = 0; i < NB; i++)
        ham_band_of((bands[i].lo + bands[i].hi) / 2, &bands[i].lo, &bands[i].hi);
    for (int i = 0; i < NB; i++)
        band_ok[i] = !cfg_tx_refusal(bands[i].lo) && !cfg_tx_refusal(bands[i].hi);
    if (!band_ok[sel_band] && next_ok_band(sel_band, 1) >= 0) sel_band = next_ok_band(sel_band, 1);
}

static void segs_load(void) {
    FILE *f = fopen(SEGS_PATH, "r");
    char  line[128];

    nsegs = 0;
    if (f) {
        while (nsegs < SEGS_MAX && fgets(line, sizeof(line), f)) {
            char *name = strtok(line, "\t"), *lo = strtok(NULL, "\t"), *hi = strtok(NULL, "\t\r\n");
            int   b;

            if (!name || !lo || !hi || name[0] == '#') continue;
            segs[nsegs].lo = (int32_t)(atof(lo) * 1000);
            segs[nsegs].hi = (int32_t)(atof(hi) * 1000);
            b = band_of(segs[nsegs].lo);
            /* inside one band the radio may transmit over, or not at all */
            if (b < 0 || !band_ok[b] || segs[nsegs].hi > bands[b].hi || segs[nsegs].hi <= segs[nsegs].lo) continue;
            snprintf(segs[nsegs].name, sizeof(segs[nsegs].name), "%s", name);
            nsegs++;
        }
        fclose(f);
    }
    if (nsegs) return;
    for (size_t i = 0; i < sizeof(segs_default) / sizeof(segs_default[0]); i++) {
        int b = band_of(segs_default[i].lo);
        if (b < 0 || !band_ok[b]) continue;
        snprintf(segs[nsegs].name, sizeof(segs[nsegs].name), "%s", segs_default[i].name);
        segs[nsegs].lo = segs_default[i].lo;
        segs[nsegs].hi = segs_default[i].hi;
        nsegs++;
    }
}

static void cal_load(void) {
    FILE *f = fopen(CAL_PATH, "r");
    char  name[16];
    int   p;
    float g;

    memset(cal_ok, 0, sizeof(cal_ok));
    if (!f) return;
    while (fscanf(f, "%15s %d %f", name, &p, &g) == 3)
        for (int b = 0; b < NB; b++)
            if (!strcmp(name, bands[b].name) && p >= 0 && p < NP) {
                cal_g[b][p] = g;
                cal_ok[b][p] = true;
            }
    fclose(f);
}

static void cal_save(void) {
    FILE *f = fopen(CAL_PATH ".tmp", "w");

    if (!f) return;
    for (int b = 0; b < NB; b++)
        for (int p = 0; p < NP; p++)
            if (cal_ok[b][p]) fprintf(f, "%s\t%d\t%.4f\n", bands[b].name, p, cal_g[b][p]);
    fclose(f);
    rename(CAL_PATH ".tmp", CAL_PATH);
}

/* The plot's x of a frequency in the sweep */
static lv_coord_t freq_x(int32_t f, lv_coord_t x0, lv_coord_t w) {
    return x0 + (int64_t)(f - f_low) * w / (f_high - f_low);
}

/* SWR to the plot's y from its top: 1 at the bottom, 5 at the top in linear */
static lv_coord_t swr_y(float v, lv_coord_t h) {
    float k;

    if (lin_scale)
        k = (v - 1.0f) / 4.0f;
    else
        k = log10f(1.0f + (v - 1.0f) / (1.0f / logf(10.0)));
    return (1.0f - LV_CLAMP(0.0f, k, 1.0f)) * h;
}

static void draw_text(lv_draw_ctx_t *ctx, lv_draw_label_dsc_t *dsc, lv_coord_t x, lv_coord_t y_mid,
                      lv_coord_t y_top, const char *s, bool centred)
{
    lv_point_t  size;
    lv_area_t   a;

    lv_txt_get_size(&size, s, dsc->font, 0, 0, LV_COORD_MAX, 0);
    if (centred) {
        a.x1 = x - size.x / 2;
        a.y1 = y_top;
        a.x2 = a.x1 + size.x;
        a.y2 = a.y1 + size.y;
    } else {
        a.x1 = x;
        a.y1 = y_mid - size.y / 2;
        a.x2 = x + size.x;
        a.y2 = y_mid + size.y / 2;
    }
    lv_draw_label(ctx, dsc, &a, s, NULL);
}

static void draw_text_right(lv_draw_ctx_t *ctx, lv_draw_label_dsc_t *dsc, lv_coord_t x, lv_coord_t y_top, const char *s) {
    lv_point_t size;

    lv_txt_get_size(&size, s, dsc->font, 0, 0, LV_COORD_MAX, 0);
    draw_text(ctx, dsc, x - size.x, y_top + size.y / 2, 0, s, false);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static lv_color_t swr_color(float v) {
    return v <= 1.5f ? TC(ok) : v <= 3.0f ? TC(warn) : TC(err);
}

/* The mean of the readings around i (two each side), NAN where none */
static float smooth_at(const float *a, int i, int n) {
    float sum = 0.0f;
    int   k = 0;

    for (int j = i - 2; j <= i + 2; j++)
        if (j >= 0 && j < n && isfinite(a[j])) {
            sum += a[j];
            k++;
        }
    return isfinite(a[i]) && k ? sum / k : NAN;
}

/* A curve of n readings over x0..x0+w: pieces between readings, none over a gap; colour: their worse end's, or one */
static void draw_curve(lv_draw_ctx_t *ctx, const float *a, int n, lv_coord_t x0, lv_coord_t w, lv_coord_t y0,
                       lv_coord_t h, lv_coord_t width, const lv_color_t *one, lv_opa_t opa)
{
    lv_draw_line_dsc_t line;
    lv_point_t         p1, p2;
    float              prev = NAN;

    lv_draw_line_dsc_init(&line);
    line.width = width;
    line.opa = opa;
    line.round_start = 1;
    line.round_end = 1;
    for (int i = 0; i < n; i++) {
        float v = smooth_at(a, i, n);

        p2.x = x0 + i * w / (n - 1);
        p2.y = y0 + swr_y(v, h);
        if (isfinite(v) && isfinite(prev)) {
            line.color = one ? *one : swr_color(LV_MAX(prev, v));
            lv_draw_line(ctx, &line, &p1, &p2);
        }
        prev = v;
        p1 = p2;
    }
}

/* The All bands overview: a column per band the radio may transmit on, its curve, its best SWR and where; the cursor
 * on the one MFK chose, the sweep's place on the band being read */
static void draw_overview(lv_draw_ctx_t *ctx) {
    lv_coord_t          x0 = plot->coords.x1 + 4, top = plot->coords.y1;
    lv_coord_t          w = lv_area_get_width(&plot->coords) - 8;
    lv_coord_t          yt = top + 34, h = lv_area_get_height(&plot->coords) - 34 - 74;
    lv_draw_rect_dsc_t  rect;
    lv_draw_label_dsc_t text;
    lv_draw_line_dsc_t  line;
    lv_area_t           a;
    lv_point_t          p1, p2;
    char                s[48];
    int                 n = 0, k = 0;

    for (int b = 0; b < NB; b++) n += band_ok[b];
    if (!n) return;
    lv_coord_t gap = 4, bw = (w - gap * (n - 1)) / n;

    lv_draw_rect_dsc_init(&rect);
    lv_draw_label_dsc_init(&text);
    lv_draw_line_dsc_init(&line);
    for (int b = 0; b < NB; b++) {
        if (!band_ok[b]) continue;
        lv_coord_t bx = x0 + k++ * (bw + gap);
        bool       sel = b == sel_band;

        /* the zones, as the single plot's */
        rect.bg_opa = LV_OPA_20;
        a.x1 = bx;
        a.x2 = bx + bw;
        a.y1 = yt + swr_y(1.5f, h);
        a.y2 = yt + h;
        rect.bg_color = TC(ok);
        lv_draw_rect(ctx, &rect, &a);
        a.y1 = yt;
        a.y2 = yt + swr_y(3.0f, h);
        rect.bg_color = TC(err);
        lv_draw_rect(ctx, &rect, &a);
        if (sel) {
            lv_draw_rect_dsc_t box;

            lv_draw_rect_dsc_init(&box);
            box.bg_opa = LV_OPA_TRANSP;
            box.border_color = TC(text_head);
            box.border_width = 2;
            box.radius = 6;
            a.x1 = bx - 2;
            a.x2 = bx + bw + 2;
            a.y1 = top + 2;
            a.y2 = yt + h + 54;
            lv_draw_rect(ctx, &box, &a);
        }
        text.font = &sony_18;
        text.color = sel ? TC(text_head) : TC(text_dim);
        draw_text(ctx, &text, bx + bw / 2, 0, top + 8, bands[b].name, true);

        bool        live = scanning && b == cur_band;
        const float *c = live ? swr : ov[b];
        float       best = NAN;
        int32_t     best_f = 0;

        if (live || ov_done[b]) {
            int32_t lo = bands[b].lo, hi = bands[b].hi;

            draw_curve(ctx, c, POINTS_ALL, bx, bw, yt, h, 3, NULL, LV_OPA_COVER);
            for (int i = 0; i < POINTS_ALL; i++)
                if (isfinite(c[i]) && !(c[i] >= best)) {
                    best = c[i];
                    best_f = lo + (int64_t)(hi - lo) * i / (POINTS_ALL - 1);
                }
        }
        if (live) {
            line.color = TC(vfo_line);
            line.width = 2;
            p1.x = p2.x = bx + pos * bw / (POINTS_ALL - 1);
            p1.y = yt;
            p2.y = yt + h;
            lv_draw_line(ctx, &line, &p1, &p2);
        }
        /* in big the band's typical SWR (the median: a lone low reading does not speak for the band), under it the
         * best and where */
        float typ = NAN;
        if (isfinite(best) && !live) {
            float v[POINTS_ALL];
            int   k = 0;

            for (int i = 0; i < POINTS_ALL; i++)
                if (isfinite(c[i])) v[k++] = c[i];
            qsort(v, k, sizeof(v[0]), cmp_float);
            typ = k % 2 ? v[k / 2] : (v[k / 2 - 1] + v[k / 2]) / 2;
        }
        text.font = &sony_22;
        if (isfinite(typ)) {
            text.color = swr_color(typ);
            snprintf(s, sizeof(s), typ >= 9.95f ? ">9" : "%.1f", typ);
        } else {
            text.color = TC(text_dim);
            snprintf(s, sizeof(s), live ? "..." : "-");
        }
        draw_text(ctx, &text, bx + bw / 2, 0, yt + h + 8, s, true);
        if (isfinite(typ)) {
            text.font = &sony_14;
            text.color = TC(text_dim);
            snprintf(s, sizeof(s), best >= 9.95f ? "min >9" : "min %.1f", best);
            draw_text(ctx, &text, bx + bw / 2, 0, yt + h + 34, s, true);
        }
        (void)best_f;
    }
    text.font = &sony_16;
    text.color = TC(text_dim);
    if (scanning && !rekey && pos < points) {
        int32_t f = freq_at(pos);

        text.color = TC(tx);
        snprintf(s, sizeof(s), "Sweeping %s: TX %d.%03d MHz", bands[cur_band].name, (int)(f / 1000000),
                 (int)(f / 1000 % 1000));
        draw_text(ctx, &text, x0 + w / 2, 0, plot->coords.y2 - 17, s, true);
    } else
    draw_text(ctx, &text, x0 + w / 2, 0, plot->coords.y2 - 17,
              range == R_CAL ? "50 ohm dummy load on the antenna socket: Run calibrates every band"
                             : "MFK: choose a band - press: a closer look", true);
}

/* Drawn over the plot's empty box: zones, grid, the three frequencies, the sweep before, the curve, its best point */
static void draw_cb(lv_event_t *e) {
    lv_draw_ctx_t       *ctx = lv_event_get_draw_ctx(e);
    lv_coord_t          x0 = plot->coords.x1;
    lv_coord_t          top = plot->coords.y1;
    lv_coord_t          y0 = top + TOP_H;
    lv_coord_t          w = lv_area_get_width(&plot->coords);
    lv_coord_t          h = lv_area_get_height(&plot->coords) - TOP_H - BOTTOM_H;
    lv_draw_rect_dsc_t  zone;
    lv_draw_line_dsc_t  line;
    lv_draw_label_dsc_t text;
    lv_area_t           a;
    lv_point_t          p1, p2;
    char                s[64];

    if (pass_range()) {
        draw_overview(ctx);
        return;
    }

    /* Zones in the theme colours: up to 1.5 good, over 3 bad */
    lv_draw_rect_dsc_init(&zone);
    zone.bg_opa = LV_OPA_20;
    a.x1 = x0;
    a.x2 = x0 + w;
    a.y1 = y0 + swr_y(1.5f, h);
    a.y2 = y0 + h;
    zone.bg_color = TC(ok);
    lv_draw_rect(ctx, &zone, &a);
    a.y1 = y0;
    a.y2 = y0 + swr_y(3.0f, h);
    zone.bg_color = TC(err);
    if (theme_shape.mono) zone.bg_opa = LV_OPA_30;   /* 1bit: the bad zone stippled (the good one goes) */
    lv_draw_rect(ctx, &zone, &a);

    /* SWR lines every 0.5, each after its number */
    lv_draw_label_dsc_init(&text);
    text.color = TC(text_dim);
    text.font = &sony_20;
    lv_draw_line_dsc_init(&line);
    line.color = TC(line);
    line.width = 1;
    line.opa = LV_OPA_60;

    for (float v = 1.5f; v <= 4.5f; v += 0.5f) {
        lv_point_t size;

        snprintf(s, sizeof(s), "%.1f", v);
        lv_txt_get_size(&size, s, text.font, 0, 0, LV_COORD_MAX, 0);
        p1.x = x0 + size.x + 6;
        p1.y = p2.y = y0 + swr_y(v, h);
        p2.x = x0 + w;
        lv_draw_line(ctx, &line, &p1, &p2);
        draw_text(ctx, &text, x0, p1.y, 0, s, false);
    }

    /* Three frequencies at the quarters of the sweep, the VFO's in the middle stronger. A sweep moved inside the TX
     * range (reset_scan) has the VFO elsewhere: its line is drawn there, without a number */
    bool vfo_drawn = false;

    for (int q = 1; q <= 3; q++) {
        int32_t f = f_low + (f_high - f_low) / 4 * q;
        bool    vfo = range == R_VFO && f == f_center;

        vfo_drawn |= vfo;
        p1.x = p2.x = x0 + w * q / 4;
        p1.y = top + 4 + 24;
        p2.y = y0 + h;
        line.color = vfo ? TC(vfo_line) : TC(line);
        line.opa = vfo ? LV_OPA_COVER : LV_OPA_60;
        lv_draw_line(ctx, &line, &p1, &p2);

        text.color = vfo ? TC(text) : TC(text_dim);
        snprintf(s, sizeof(s), "%i.%03i.%03i", (int)(f / 1000000), (int)(f / 1000 % 1000), (int)(f % 1000));
        draw_text(ctx, &text, p1.x, 0, top + 4, s, true);
    }
    if (range == R_VFO && !vfo_drawn && f_center >= f_low && f_center <= f_high) {
        p1.x = p2.x = freq_x(f_center, x0, w);
        line.color = TC(vfo_line);
        line.opa = LV_OPA_COVER;
        lv_draw_line(ctx, &line, &p1, &p2);
    }

    /* A VFO sweep that leaves the amateur bands (TX outside ham bands on) says so, in red */
    if (range == R_VFO && (band_of(f_low) < 0 || band_of(f_low) != band_of(f_high))) {
        text.color = TC(err);
        draw_text(ctx, &text, x0 + 60, y0 + 16, 0, "OUT OF BAND TX", false);
    }

    /* What is swept, top left: the band or the segment, and whether the readings are calibrated */
    text.color = TC(text_head);
    snprintf(s, sizeof(s), "%s%s", range == R_BAND ? bands[sel_band].name : range == R_SEG ? segs[sel_seg].name : "",
             band_of(f_low) >= 0 && cal_ok[band_of(f_low)][pwr_idx()] ? "  cal" : "");
    if (s[0]) draw_text(ctx, &text, x0 + 60, y0 + 16, 0, s, false);

    /* The frequency on the air while it sweeps: the strip above keeps the VFO's, where Stop goes back to */
    if (scanning && !rekey && pos < points) {
        int32_t f = freq_at(pos);

        text.color = TC(tx);
        snprintf(s, sizeof(s), "TX %d.%03d.%03d", (int)(f / 1000000), (int)(f / 1000 % 1000), (int)(f % 1000));
        draw_text(ctx, &text, x0 + 60, y0 + 44, 0, s, false);
    }

    /* The sweep before, faint, then this one */
    if (have_ghost) {
        lv_color_t c = TC(text_mute);
        draw_curve(ctx, ghost, points, x0, w, y0, h, 3, &c, LV_OPA_70);
    }
    draw_curve(ctx, swr, points, x0, w, y0, h, 4, NULL, LV_OPA_COVER);

    /* The best point of the last whole sweep (this one before it has one), and the width under 2:1 */
    const float *src = have_ghost ? ghost : swr;
    float        best = NAN;
    int          bi = -1, lo2 = -1, hi2 = -1;

    for (int i = 0; i < points; i++) {
        float v = smooth_at(src, i, points);

        if (!isfinite(v)) continue;
        if (!(v >= best)) {
            best = v;
            bi = i;
        }
        if (v < 2.0f) {
            if (lo2 < 0) lo2 = i;
            hi2 = i;
        }
    }
    if (bi >= 0) {
        /* the middle of the bottom: the base reads in tenths, a dip is often flat over many points */
        int a = bi, z = bi;

        while (a > 0 && smooth_at(src, a - 1, points) <= best + 0.05f) a--;
        while (z < points - 1 && smooth_at(src, z + 1, points) <= best + 0.05f) z++;
        bi = (a + z) / 2;

        int32_t f = freq_at(bi);

        p1.x = p2.x = x0 + bi * w / (points - 1);
        p1.y = y0;
        p2.y = y0 + h;
        line.color = TC(text_head);
        line.opa = LV_OPA_COVER;
        lv_draw_line(ctx, &line, &p1, &p2);
        text.color = swr_color(best);
        text.font = &sony_22;
        if (best >= 9.95f)
            snprintf(s, sizeof(s), "min >9.9 at %d.%03d MHz", (int)(f / 1000000), (int)(f / 1000 % 1000));
        else
            snprintf(s, sizeof(s), "min %.2f at %d.%03d MHz", best, (int)(f / 1000000), (int)(f / 1000 % 1000));
        draw_text_right(ctx, &text, x0 + w - 8, y0 + 4, s);
        text.font = &sony_18;
        text.color = TC(text);
        if (lo2 >= 0)
            snprintf(s, sizeof(s), "< 2:1  %d kHz wide", (int)((freq_at(hi2) - freq_at(lo2)) / 1000));
        else
            snprintf(s, sizeof(s), "nowhere under 2:1");
        draw_text_right(ctx, &text, x0 + w - 8, y0 + 32, s);
    }
}

/* What a sweep is called, in the CSV's name and for the Panel */
static void sweep_label(char *buf, size_t n, int band) {
    if (band >= 0)
        snprintf(buf, n, "%s", bands[band].name);
    else if (range == R_SEG)
        snprintf(buf, n, "%s", segs[sel_seg].name);
    else if (range == R_BAND)
        snprintf(buf, n, "%s", bands[sel_band].name);
    else
        snprintf(buf, n, "VFO-%d", (int)(f_center / 1000));
    for (char *p = buf; *p; p++)
        if (*p == ' ' || *p == '/') *p = '_';
}

/* A sweep to the Panel pages: "swr {"name", "lo", "hi", "w" (power), "v": [SWR or null]}" */
static void sweep_send(int band, int32_t lo, int32_t hi, const float *v, int n) {
    char *json = malloc(96 + n * 7), name[32];
    int   k;

    if (!json) return;
    sweep_label(name, sizeof(name), band);
    k = sprintf(json, "{\"name\":\"%s\",\"lo\":%d,\"hi\":%d,\"w\":%.0f,\"v\":[", name, (int)lo, (int)hi, scan_pwr());
    for (int i = 0; i < n; i++)
        k += isfinite(v[i]) ? sprintf(json + k, "%s%.2f", i ? "," : "", v[i]) : sprintf(json + k, "%snull", i ? "," : "");
    strcpy(json + k, "]}");
    remote_server_swr(json);
    free(json);
}

/* SWEEPS_DIR/YYYYMMDD-HHMMSS_<name>.csv: one line a point, "frequency;band;SWR" (empty SWR where not read) */
static void sweep_save(const char *name, int nb, const int *band, const int32_t *lo, const int32_t *hi,
                       const float *const *v, const float *const *p, const int *n) {
    char       path[128];
    time_t     now = time(NULL);
    struct tm  tm;
    FILE      *f;

    gmtime_r(&now, &tm);
    mkdir(SWEEPS_DIR, 0755);
    snprintf(path, sizeof(path), SWEEPS_DIR "/%04d%02d%02d-%02d%02d%02d_%s.csv", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, name);
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "# SWR scan, %.0f W%s\nfrequency_hz;band;swr;power_w\n", scan_pwr(),
            band_of(lo[0]) >= 0 && cal_ok[band_of(lo[0])][pwr_idx()] ? ", calibrated" : "");
    for (int b = 0; b < nb; b++)
        for (int i = 0; i < n[b]; i++) {
            int32_t fr = lo[b] + (int64_t)(hi[b] - lo[b]) * i / (n[b] - 1);
            if (isfinite(v[b][i]))
                fprintf(f, "%d;%s;%.2f;%.1f\n", (int)fr, band[b] >= 0 ? bands[band[b]].name : "", v[b][i], p[b][i]);
            else
                fprintf(f, "%d;%s;;\n", (int)fr, band[b] >= 0 ? bands[band[b]].name : "");
        }
    fclose(f);
}

/* The pass's bands in one file */
static void pass_save(void) {
    int          nb = 0, band[NB], n[NB];
    int32_t      lo[NB], hi[NB];
    const float *v[NB], *p[NB];

    for (int b = 0; b < NB; b++)
        if (ov_done[b]) {
            band[nb] = b; lo[nb] = bands[b].lo; hi[nb] = bands[b].hi; v[nb] = ov[b]; p[nb] = ov_pwr[b]; n[nb] = POINTS_ALL;
            nb++;
        }
    if (nb) sweep_save(range == R_CAL ? "calibration" : "all-bands", nb, band, lo, hi, v, p, n);
}

/* The last whole sweep of a VFO, Band or Segment run, at Stop */
static void run_save(void) {
    char         name[32];
    int          band[1] = {-1}, n[1] = {points};
    int32_t      lo[1] = {f_low}, hi[1] = {f_high};
    const float *v[1] = {ghost}, *p[1] = {ghost_pwr};

    if (!have_ghost || pass_range()) return;
    sweep_label(name, sizeof(name), -1);
    sweep_save(name, 1, band, lo, hi, v, p, n);
}

static void rekey_cb(lv_timer_t *t) {
    rekey = NULL;
    if (!scanning) return;
    settle_n = SETTLE_N;
    settle_v = NAN;
    settle_alike = SETTLE_ALIKE;
    if (!radio_start_swrscan(freq_at(pos))) dialog_swrscan_give_up("SWR scan: could not key the next band");
}

/* On to f with the carrier off on the way: the filter relays switch unkeyed, the carrier comes back after REKEY_MS */
static void move_unkeyed(int32_t f) {
    radio_stop_swrscan();
    radio_set_tx_freq(f);
    rekey = lv_timer_create(rekey_cb, REKEY_MS, NULL);
    lv_timer_set_repeat_count(rekey, 1);
}

/* From one point to the next: keyed inside one band; between two bands, or outside them (where the radio's filters
 * change anywhere), unkeyed */
static void move_to(int32_t from, int32_t to) {
    int b = band_of(to);

    if (b < 0 || b != band_of(from))
        move_unkeyed(to);
    else
        radio_set_tx_freq(to);
}

/* The pass over every band done: Calibrate keeps the bridge's reflection on the load, per band at this power */
static void pass_done(void) {
    bool cal = range == R_CAL;
    int  n = 0, bad = 0;

    pass_save();
    run_cb(NULL);
    if (cal) {
        for (int b = 0; b < NB; b++) {
            float sum = 0.0f;
            int   k = 0;

            if (!ov_done[b]) continue;
            for (int i = 0; i < POINTS_ALL; i++)
                if (isfinite(ov[b][i])) {
                    sum += (ov[b][i] - 1.0f) / (ov[b][i] + 1.0f);
                    k++;
                }
            if (!k) continue;
            if (sum / k > CAL_MAX_G) {      /* not a dummy load there: an antenna, an open socket. Kept as it was */
                bad++;
                continue;
            }
            cal_g[b][pwr_i] = sum / k;
            cal_ok[b][pwr_i] = true;
            n++;
        }
        cal_save();
        if (bad)
            msg_update_text_fmt("%d bands over 1.5: no 50 ohm load there, not kept", bad);
        else
            msg_update_text_fmt("Calibrated: %d bands at %.0f W", n, pwr_w[pwr_i]);
    } else {
        msg_update_text_fmt("All bands done");
    }
}

/* A sweep done. The passes go on to the next band, unkeyed between the two; the others start again, the sweep kept
 * faint under the next */
static void sweep_done(void) {
    if (pass_range()) {
        int nb;

        memcpy(ov[cur_band], swr, sizeof(ov[cur_band]));
        memcpy(ov_pwr[cur_band], pwr, sizeof(ov_pwr[cur_band]));
        ov_done[cur_band] = true;
        sweep_send(cur_band, bands[cur_band].lo, bands[cur_band].hi, ov[cur_band], POINTS_ALL);
        nb = next_ok_band(cur_band, 1);
        if (nb <= cur_band) {
            pass_done();
            return;
        }
        set_band_sweep(nb);
        for (int i = 0; i < POINTS_MAX; i++) swr[i] = NAN;
        pos = next_point(0);
        move_unkeyed(freq_at(pos));
        return;
    }
    int32_t from = freq_at(points - 1);

    sweep_send(-1, f_low, f_high, swr, points);
    memcpy(ghost, swr, sizeof(ghost));
    memcpy(ghost_pwr, pwr, sizeof(ghost_pwr));
    have_ghost = true;
    for (int i = 0; i < POINTS_MAX; i++) swr[i] = NAN;
    pos = next_point(0);
    move_to(from, freq_at(pos));
}

/* One reading at the current point, then on to the next frequency */
static void scan_step(float v, float p) {
    if (rekey || pos >= points) return;
    if (settle_n > 0) {         /* the carrier just came: the point read again once the power is up */
        settle_n--;
        return;
    }
    if (settle_alike > 0) {
        bool alike = isfinite(settle_v) && fabsf(v - settle_v) <= 0.15f;

        settle_v = v;
        if (!alike && --settle_alike > 0) return;
        settle_alike = 0;
    }
    if (v < 1.0f) v = NAN;      /* no reading: no power came out (the band's first packets, a base that does not say) */
    v = corrected(v, freq_at(pos));
    swr[pos] = v;
    pwr[pos] = p;
    if (scan_pwr() > pwr_w[0] && isfinite(v) && v > GUARD_SWR) {
        if (++guard_n >= GUARD_N) {     /* the sweep goes on at 1 W: a band's edges often read over 3 */
            pwr_i = 0;
            guard_n = 0;
            radio_swrscan_power(pwr_w[0]);
            msg_update_text_fmt("SWR over 3: power back to 1 W");
            buttons_refresh(&btn_power);
        }
    } else {
        guard_n = 0;
    }
    event_send(plot, LV_EVENT_REFRESH, NULL);

    int32_t from = freq_at(pos);

    pos = next_point(pos + 1);
    if (pos >= points)
        sweep_done();
    else
        move_to(from, freq_at(pos));
}

/* The dial moved: the VFO range sweeps around it again; the others stay where they are. Moved where the radio may not
 * transmit (#121), the scan stops, as if Stop was pressed: the VFO settings back as before the scan, the dial with
 * them */
static void freq_changed(Subject *subj, void *user_data) {
    int32_t tx = radio_tx_freq();

    if (scanning && cfg_tx_refusal(tx)) {
        dialog_swrscan_stop(tx);
        return;
    }
    if (range != R_VFO || rekey) return;
    reset_scan();
    lv_obj_invalidate(plot);
}

static void linear_changed(Subject *subj, void *user_data) {
    lin_scale = subject_i_get((SubjectInt *)subj);
}

static void span_changed(Subject *subj, void *user_data) {
    span_hz = subject_i_get((SubjectInt *)subj);
}

/* An empty box the size of the dialog: draw_cb paints it, it takes the keys */
static lv_obj_t *make_plot(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_add_event_cb(box, draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_set_size(box, 780, DIALOG_H - 8);    /* the frequencies on top stay inside the dialog */
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_group_add_obj(keyboard_group, box);
    lv_obj_add_event_cb(box, plot_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(box);
    lv_group_set_editing(keyboard_group, true);     /* MFK turns as LEFT/RIGHT to the plot, as in Field Ops */
    return box;
}

static void open_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    /* the Scale label follows its setting, the plot reads it and the span too */
    btn_scale.subj = (Subject *)cfg_swrscan_linear;
    linear_obs = subject_subscribe_delayed_and_notify((Subject *)cfg_swrscan_linear, linear_changed, NULL);
    span_obs = subject_subscribe_delayed_and_notify((Subject *)cfg_swrscan_span, span_changed, NULL);
    pwr_i = 0;
    radio_swrscan_power(pwr_w[0]);
    bands_check();
    segs_load();
    cal_load();
    if (range == R_SEG && sel_seg >= nsegs) sel_seg = 0;
    if (range == R_SEG && !nsegs) range = R_VFO;
    buttons_load_page(&page);

    freq_obs = subject_subscribe_delayed((Subject *)cfg_fg_freq, freq_changed, NULL);
    plot = make_plot(dialog.obj);
    reset_scan();
}

static void unsubscribe(ObserverDelayed **obs) {
    if (*obs) param_unsubscribe((Observer *)*obs);
    *obs = NULL;
}

/* A scan still running stops first; the VFO goes back to its frequency */
static void close_cb(void) {
    if (scanning) run_cb(NULL);
    unsubscribe(&freq_obs);
    unsubscribe(&linear_obs);
    unsubscribe(&span_obs);
    radio_swrscan_power(pwr_w[0]);
    radio_set_tx_freq(0);
}

/* MFK: the overview's cursor, the band or the segment swept (not while scanning); its press: a closer look at the
 * band chosen, or back to the overview */
static void mfk_turn(int d) {
    if (scanning && range != R_VFO) return;
    if (pass_range() || range == R_BAND) {
        int b = next_ok_band(sel_band, d);
        if (b >= 0) sel_band = b;
    } else if (range == R_SEG && nsegs) {
        sel_seg = (sel_seg + d + nsegs) % nsegs;
    } else {
        main_screen_mfk(d);     /* VFO: MFK as on the main screen */
        return;
    }
    if (!pass_range()) reset_scan();
    buttons_refresh(&btn_pick);
    lv_obj_invalidate(plot);
}

static void zoom_toggle(void) {
    if (scanning) return;
    if (range == R_ALL)
        range = R_BAND;
    else if (range == R_BAND)
        range = R_ALL;
    else
        return;
    if (range == R_BAND) reset_scan();
    buttons_refresh(&btn_range);
    buttons_refresh(&btn_pick);
    buttons_refresh(&btn_power);
    lv_obj_invalidate(plot);
}

static void plot_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
    else if (key == LV_KEY_LEFT || key == LV_KEY_UP)
        mfk_turn(-1);
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN)
        mfk_turn(1);
    else if (key == LV_KEY_ENTER)
        zoom_toggle();
}

/* Run / stop. The VFO settings are kept aside while scanning and put back after; the sweep moves the VFO that
 * transmits, which goes back to its own frequency. A start that cannot be (a tune or TX on, a receive-only
 * frequency) says why and moves nothing: it used to keep "Run" and move the VFO to the low edge in silence (#25) */
static void run_cb(button_data_t *b) {
    if (scanning) {
        run_save();
        scanning = false;
        if (rekey) {
            lv_timer_del(rekey);
            rekey = NULL;
        }
        radio_stop_swrscan();
        radio_set_tx_freq(0);
        mem_load(MEM_BACKUP_ID);
    } else if (radio_get_state() != RADIO_RX) {
        msg_update_text_fmt("SWR scan: not while transmitting or tuning");
    } else if (pass_range() && next_ok_band(-1, 1) < 0) {
        msg_update_text_fmt("SWR scan: no band the radio may transmit on");
    } else {
        mem_save(MEM_BACKUP_ID);
        reset_scan();
        if (pass_range()) memset(ov_done, 0, sizeof(ov_done));
        radio_swrscan_power(scan_pwr());
        settle_n = SETTLE_N;
    settle_v = NAN;
    settle_alike = SETTLE_ALIKE;
        scanning = radio_start_swrscan(freq_at(pos));
        if (!scanning) mem_load(MEM_BACKUP_ID);
    }
    buttons_refresh(&btn_run);
    lv_obj_invalidate(plot);
}

static void range_cb(button_data_t *b) {
    if (scanning) return;
    range = (range + 1) % R_COUNT;
    if (range == R_SEG && !nsegs) range = (range + 1) % R_COUNT;
    reset_scan();
    buttons_refresh(&btn_range);
    buttons_refresh(&btn_pick);
    buttons_refresh(&btn_power);
    lv_obj_invalidate(plot);
}

/* VFO: the next span, from 20 kHz (+-10, to trim an antenna by eye) up to 2500 kHz as the stock firmware has them
 * (#107), 5 and 10 MHz with TX outside ham bands on, told as the VFO plus or minus half of it; All bands: the closer
 * look; Band and Segment: the next one */
static void pick_cb(button_data_t *b) {
    static const int32_t spans[] = { 20000, 50000, 100000, 200000, 500000, 1000000, 1500000, 2000000, 2500000,
                                     5000000, 10000000 };
    const int            n = sizeof(spans) / sizeof(spans[0]) - (outside_on() ? 0 : 2);   /* 5, 10 MHz: outside */
    int32_t              cur, next = spans[0];

    if (scanning) return;
    if (range == R_ALL) {
        zoom_toggle();
        return;
    }
    if (range != R_VFO) {
        mfk_turn(1);
        return;
    }
    cur = param_i_get(cfg_swrscan_span);
    for (int i = 0; i < n; i++) {
        if (spans[i] == cur) {
            next = spans[(i + 1) % n];
            break;
        }
    }
    param_i_set(cfg_swrscan_span, next);

    span_hz = next;     /* the observer is delayed: the sweep and its frequencies follow now */
    reset_scan();
    buttons_refresh(&btn_pick);
    event_send(plot, LV_EVENT_REFRESH, NULL);
}

static void power_cb(button_data_t *b) {
    if (scanning) return;
    pwr_i = (pwr_i + 1) % NP;
    buttons_refresh(&btn_power);
    lv_obj_invalidate(plot);
}

static void scale_cb(button_data_t *b) {
    param_i_set(cfg_swrscan_linear, !param_i_get(cfg_swrscan_linear));
}

static const char *run_label() {
    return scanning ? "Stop" : "Run";
}

static const char *range_label() {
    static const char *names[R_COUNT] = {"Range:\nVFO", "Range:\nAll bands", "Range:\nBand", "Range:\nSegment",
                                         "Range:\nCalibrate"};
    return names[range];
}

static const char *pick_label() {
    static char s[40];

    switch (range) {
        case R_VFO:  snprintf(s, sizeof(s), "Span:\n+/-%u kHz", param_i_get(cfg_swrscan_span) / 2000); break;
        case R_ALL:  snprintf(s, sizeof(s), "Zoom:\n%s", bands[sel_band].name); break;
        case R_BAND: snprintf(s, sizeof(s), "Band:\n%s", bands[sel_band].name); break;
        case R_SEG:  snprintf(s, sizeof(s), "Segment:\n%s", nsegs ? segs[sel_seg].name : "-"); break;
        default:     snprintf(s, sizeof(s), "Load:\n50 ohm"); break;
    }
    return s;
}

static const char *power_label() {
    static char s[20];

    snprintf(s, sizeof(s), "Power:\n%.0f W", pwr_w[pwr_i]);
    return s;
}

static const char *scale_label() {
    return param_i_get(cfg_swrscan_linear) ? "Scale:\nLinear" : "Scale:\nLog";
}

static void step_cb(void *arg) {
    float *r = arg;

    atomic_store(&step_due, false);
    if (scanning) scan_step(r[0], r[1]);
}

/* The radio thread's reading: the step, the moves and the stops it may lead to go on the GUI thread, one at a time */
void dialog_swrscan_update(float vswr, float power) {
    float r[2] = {vswr, power};

    if (!scanning || atomic_exchange(&step_due, true)) return;
    if (!scheduler_put(step_cb, r, sizeof(r))) atomic_store(&step_due, false);
}

void dialog_swrscan_stop(int32_t freq) {
    if (!scanning) return;
    run_cb(NULL);
    if (!radio_say_no_tx("Scan stopped", freq, true)) msg_update_text_fmt("Scan stopped");   /* fits whole (#121) */
}

void dialog_swrscan_give_up(const char *why) {
    if (!scanning) return;
    run_cb(NULL);
    msg_update_text_fmt("%s", why);
}
