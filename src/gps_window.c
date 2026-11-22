/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - GPS / Clock screen, two views (F4OLI's redesign, 22/11/2026). Clock: the time, what keeps
 *  it, and the sources in their order (NTP, GPS, a time station, FT8; the clock chip only the start). Station: a time
 *  station heard, its minute on a dial with the next marker, the tone's level, the markers heard and whether they
 *  agree, the hour's marker minutes. View changes the view; Listen goes to Station.
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY, own code of the IU1WXY firmware
 */

#include "gps_window.h"

#include "gps.h"
#include "gui_styles.h"
#include "gui_events.h"
#include "radio.h"
#include "key_input.h"
#include "qth/qth.h"
#include "clock_sync.h"
#include "clock_beacon.h"
#include "freq_list.h"
#include "msg_box.h"
#include "theme.h"
#include "cfg/cfg_api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W           770
#define LADDER_X    396
#define LADDER_W    368
#define SRC_N       4
#define HIST_X      216
#define HIST_W      552
#define DIAL_R      90
#define SEGS        30

static lv_obj_t   *win;
static lv_obj_t   *view_clock, *view_station;
static bool        station_view;
static lv_timer_t *status_timer;
static int         station = 1;     /* RWM 9996: Moscow, heard all over Europe */
static bool        tuned;           /* the radio given back on close */
static unsigned    result_seen;

/* Clock view */
static lv_obj_t *l_time, *l_date, *keep_box, *l_keep_name, *l_keep_detail, *l_gps, *l_ft8;
static lv_obj_t *src_box[SRC_N], *src_name[SRC_N], *src_detail[SRC_N], *src_tag[SRC_N];

/* Station view */
static lv_obj_t *l_st_name, *l_st_freq, *chip, *l_chip, *l_holder, *dial, *l_next, *tone_bar, *l_tone, *sched;
static lv_obj_t *hist_box[CLOCK_BEACON_HIST], *hist_t[CLOCK_BEACON_HIST], *hist_off[CLOCK_BEACON_HIST],
    *hist_db[CLOCK_BEACON_HIST], *hist_ok[CLOCK_BEACON_HIST];
static float     tone_db;

/* GPS, from gpsd's reports */
static char gps_text[64] = "";

static void open_cb(lv_obj_t *parent);
static void close_cb(void);
static void window_key_cb(lv_event_t *e);

static const char *view_label(void);
static const char *station_label(void);
static const char *listen_label(void);
static const char *ft8_auto_label(void);
static void view_cb(button_data_t *btn);
static void station_cb(button_data_t *btn);
static void listen_cb(button_data_t *btn);
static void ft8_set_cb(button_data_t *btn);
static void ft8_auto_cb(button_data_t *btn);

static button_data_t btn_view     = {.type = BTN_TEXT_FN, .label_fn = view_label, .press = view_cb};
static button_data_t btn_station  = {.type = BTN_TEXT_FN, .label_fn = station_label, .press = station_cb};
static button_data_t btn_listen   = {.type = BTN_TEXT_FN, .label_fn = listen_label, .press = listen_cb};
static button_data_t btn_ft8_set  = {.type = BTN_TEXT, .label = "Set from\nFT8", .press = ft8_set_cb};
static button_data_t btn_ft8_auto = {.type = BTN_TEXT_FN, .label_fn = ft8_auto_label, .press = ft8_auto_cb};

static buttons_page_t page = {{&btn_view, &btn_station, &btn_listen, &btn_ft8_set, &btn_ft8_auto}};

static dialog_t dialog = {
    .construct_cb = open_cb, .destruct_cb = close_cb, .key_cb = window_key_cb, .btn_page = &page
};

dialog_t *dialog_gps = &dialog;

/* ---- small parts ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t bg) {
    lv_obj_t *b = lv_obj_create(parent);

    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_border_color(b, TC(ok), 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

/* A source's box green and framed when it keeps the clock */
static void lit(lv_obj_t *b, bool on) {
    lv_obj_set_style_bg_color(b, on ? lv_color_mix(TC(ok), TC(row), 60) : TC(row), 0);
    lv_obj_set_style_border_width(b, on ? 2 : 0, 0);
}

/* "3 min ago" */
static void ago(char *buf, size_t size, time_t when) {
    long s = (long)(time(NULL) - when);

    if (s < 90)
        snprintf(buf, size, "%ld s ago", s < 0 ? 0 : s);
    else if (s < 90 * 60)
        snprintf(buf, size, "%ld min ago", s / 60);
    else
        snprintf(buf, size, "%ld h ago", s / 3600);
}

static void hhmm(char *buf, size_t size, time_t t) {
    struct tm tm;

    gmtime_r(&t, &tm);
    snprintf(buf, size, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* One report from gpsd (gps.c): satellites, fix, grid */
static void report_cb(lv_event_t *e) {
#if HAVE_LIBGPS
    struct gps_data_t *g   = lv_event_get_param(e);
    const char        *fix = g->fix.mode == MODE_3D ? "3D" : g->fix.mode == MODE_2D ? "2D" : "no fix";
    char               grid[16] = "";

    if (g->fix.mode >= MODE_2D)
        qth_pos_to_str(g->fix.latitude, g->fix.longitude, grid);
    snprintf(gps_text, sizeof(gps_text), "%s, %i/%i satellites%s%s", fix, g->satellites_used, g->satellites_visible,
             grid[0] ? ", " : "", grid);
#else
    (void)e;
#endif
}

/* ---- the clock: who keeps it ---- */

enum { SRC_NTP, SRC_GPS, SRC_STATION, SRC_FT8, SRC_CHIP };

/* The first source in the order that keeps or set the clock since the start */
static int holder(void) {
    double lat, lon, step;
    int    age;
    char   by[24];
    time_t when;

    if (clock_sync_ntp())
        return SRC_NTP;
    if (gps_last_fix(&lat, &lon, &age) && age <= 10)
        return SRC_GPS;
    if (clock_sync_station(by, sizeof(by), &when, &step))
        return SRC_STATION;
    if (clock_sync_last(by, sizeof(by), &when, &step) && !strcmp(by, "FT8"))
        return SRC_FT8;
    return SRC_CHIP;
}

static void clock_view_update(void) {
    time_t      now = time(NULL), when;
    struct tm   utc;
    char        by[24], a[24], t[8], text[96];
    double      step, lat, lon;
    int         age, h = holder();
    clock_ft8_t e;
    clock_beacon_state_t st;
    static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    gmtime_r(&now, &utc);
    lv_label_set_text_fmt(l_time, "%02d:%02d:%02d", utc.tm_hour, utc.tm_min, utc.tm_sec);
    lv_label_set_text_fmt(l_date, "UTC  -  %s %02d/%02d/%04d", wd[utc.tm_wday], utc.tm_mday, utc.tm_mon + 1,
                          utc.tm_year + 1900);

    bool gps_fix = gps_last_fix(&lat, &lon, &age) && age <= 10;
    bool station = clock_sync_station(by, sizeof(by), &when, &step);

    clock_sync_ft8_get(&e);
    clock_beacon_get(&st);

    /* the box: who keeps it */
    switch (h) {
        case SRC_NTP:
            lv_label_set_text(l_keep_name, "NTP (WiFi)");
            lv_label_set_text(l_keep_detail, "synchronised by ntpd");
            break;
        case SRC_GPS:
            lv_label_set_text(l_keep_name, "GPS");
            lv_label_set_text_fmt(l_keep_detail, "a fix %d s ago, through ntpd", age);
            break;
        case SRC_STATION:
            hhmm(t, sizeof(t), when);
            lv_label_set_text(l_keep_name, by);
            lv_label_set_text_fmt(l_keep_detail, "set %+.3f s at %s, holds till power-off", step, t);
            break;
        case SRC_FT8:
            clock_sync_last(by, sizeof(by), &when, &step);
            ago(a, sizeof(a), when);
            lv_label_set_text(l_keep_name, "FT8");
            lv_label_set_text_fmt(l_keep_detail, "set %+.2f s %s", step, a);
            break;
        default:
            lv_label_set_text(l_keep_name, "Clock chip");
            lv_label_set_text(l_keep_detail, "not set since the start");
            break;
    }
    lit(keep_box, h != SRC_CHIP);
    lv_obj_set_style_text_color(l_keep_name, h != SRC_CHIP ? TC(ok) : TC(warn), 0);

    lv_label_set_text_fmt(l_gps, "GPS   %s", gps_text[0] ? gps_text :
                          gps_status() == GPS_STATUS_WORKING ? "no fix" : "none plugged in");
    if (e.valid && now - e.when < 15 * 60)
        snprintf(text, sizeof(text), "%s %.2f s, %d decodes", e.ahead >= 0 ? "ahead" : "behind", fabsf(e.ahead), e.n);
    else if (clock_sync_ntp())
        snprintf(text, sizeof(text), "learning in FT8: %d slots", e.ref_slots);
    else
        snprintf(text, sizeof(text), "no measure: open FT8%s", e.ref_slots ? "" : " (not learned)");
    lv_label_set_text_fmt(l_ft8, "FT8   %s", text);

    /* the sources in their order */
    lv_label_set_text(src_detail[SRC_NTP], clock_sync_ntp() ? "synchronised" : "no WiFi, or not synchronised");
    lv_label_set_text(src_detail[SRC_GPS], gps_fix ? gps_text : gps_status() == GPS_STATUS_WORKING ? "no fix"
                                                                                                 : "none plugged in");
    if (station) {
        hhmm(t, sizeof(t), when);
        lv_label_set_text_fmt(src_detail[SRC_STATION], "%s: %+.3f s at %s", by, step, t);
    } else if (st.on) {
        lv_label_set_text_fmt(src_detail[SRC_STATION], "%s: %s", clock_beacons[st.beacon].name,
                              h < SRC_STATION ? "learning the delay" : "listening");
    } else {
        lv_label_set_text(src_detail[SRC_STATION], "not heard since the start");
    }
    lv_label_set_text_fmt(src_detail[SRC_FT8], "%s%s", text,
                          h < SRC_FT8 && e.valid && now - e.when < 15 * 60 ? ": left alone" : "");

    for (int i = 0; i < SRC_N; i++) {
        bool on = i == h;
        const char *tag;

        lit(src_box[i], on);
        if (on)
            tag = "KEEPS IT";
        else if (i < h || (i == SRC_GPS && !gps_fix) || (i == SRC_NTP && !clock_sync_ntp()))
            tag = "-";
        else if (i == SRC_FT8 && !param_i_get(cfg_clock_ft8_auto))
            tag = "off";
        else
            tag = "waits";
        lv_label_set_text(src_tag[i], tag);
        lv_obj_set_style_text_color(src_tag[i], on ? TC(ok) : TC(text_mute), 0);
        lv_obj_set_style_text_color(src_name[i], on || i > h || (i == SRC_GPS && gps_fix) ? TC(text) : TC(text_mute),
                                    0);
    }
}

/* ---- the station: a result sets the clock, or teaches the delay ---- */

static void beacon_result(const clock_beacon_state_t *st) {
    const char *why;

    if (!st->result || st->result_id == result_seen)
        return;
    result_seen = st->result_id;
    if (clock_sync_kept(&why)) {
        clock_sync_beacon_learn(-st->raw);
        msg_update_text_fmt("%s keeps the clock: the marker's delay learned (%.3f s)", why, -st->raw);
    } else if (fabsf(st->ahead) < 0.02f) {
        msg_update_text_fmt("Clock right by %s (%+.3f s)", clock_beacons[st->beacon].name, st->ahead);
    } else if (clock_sync_step(-st->ahead, clock_beacons[st->beacon].name)) {
        msg_update_text_fmt("Clock %+.3f s from %s", -st->ahead, clock_beacons[st->beacon].name);
    }
}

/* The minute on a dial: the marker at :00 (orange), the second hand */
static void dial_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t     *ctx = lv_event_get_draw_ctx(e);
    lv_area_t          a;
    lv_draw_line_dsc_t ln;
    lv_draw_arc_dsc_t  arc;
    struct timespec    now;

    lv_obj_get_coords(dial, &a);

    lv_point_t c = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};

    lv_draw_arc_dsc_init(&arc);
    arc.color = TC(line);
    arc.width = 3;
    lv_draw_arc(ctx, &arc, &c, DIAL_R, 0, 360);

    lv_draw_line_dsc_init(&ln);
    for (int s = 0; s < 60; s++) {
        double     an = (s * 6 - 90) * M_PI / 180;
        int        r1 = DIAL_R - (s % 5 ? 6 : 12);
        lv_point_t p1 = {c.x + (lv_coord_t)lround(r1 * cos(an)), c.y + (lv_coord_t)lround(r1 * sin(an))};
        lv_point_t p2 = {c.x + (lv_coord_t)lround(DIAL_R * cos(an)), c.y + (lv_coord_t)lround(DIAL_R * sin(an))};

        ln.color = s % 5 ? TC(text_mute) : TC(text);
        ln.width = s % 5 ? 1 : 2;
        lv_draw_line(ctx, &ln, &p1, &p2);
    }
    /* 15 and 45: the seconds read at a glance */
    {
        lv_draw_label_dsc_t ld;
        static const char  *q[] = {"0", "15", "30", "45"};

        lv_draw_label_dsc_init(&ld);
        ld.font  = &sony_16;
        ld.color = TC(text_mute);
        ld.align = LV_TEXT_ALIGN_CENTER;
        for (int i = 0; i < 4; i++) {
            double     an = (i * 90 - 90) * M_PI / 180;
            lv_coord_t x  = c.x + (lv_coord_t)lround((DIAL_R - 26) * cos(an)), y = c.y + (lv_coord_t)lround((DIAL_R - 26) * sin(an));
            lv_area_t  la = {x - 16, y - 9, x + 16, y + 9};

            if (i % 2 == 0)
                continue;   /* 0: the marker stands there; 30: the countdown */
            lv_draw_label(ctx, &ld, &la, q[i], NULL);
        }
    }
    /* the marker */
    ln.color = TC(warn);
    ln.width = 8;
    {
        lv_point_t p1 = {c.x, c.y}, p2 = {c.x, c.y - DIAL_R + 6};
        lv_draw_line(ctx, &ln, &p1, &p2);
    }
    /* the second hand */
    clock_gettime(CLOCK_REALTIME, &now);

    double     sec = now.tv_sec % 60 + now.tv_nsec / 1e9, an = (sec * 6 - 90) * M_PI / 180;
    lv_point_t p1 = {c.x, c.y}, p2 = {c.x + (lv_coord_t)lround((DIAL_R - 16) * cos(an)),
                                      c.y + (lv_coord_t)lround((DIAL_R - 16) * sin(an))};

    ln.color = TC(info);
    ln.width = 3;
    lv_draw_line(ctx, &ln, &p1, &p2);
}

/* The tone's level as segments, green then orange at the top */
static void tone_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t     *ctx = lv_event_get_draw_ctx(e);
    lv_area_t          a, s;
    lv_draw_rect_dsc_t r;
    int                lit_n = (int)lroundf(fminf(fmaxf(tone_db, 0), 40) / 40 * SEGS);

    lv_obj_get_coords(tone_bar, &a);
    lv_draw_rect_dsc_init(&r);
    for (int i = 0; i < SEGS; i++) {
        s.x1 = a.x1 + i * 9;
        s.x2 = s.x1 + 6;
        s.y1 = a.y1;
        s.y2 = a.y2;
        r.bg_color = i >= lit_n ? TC(row) : i < SEGS * 4 / 5 ? TC(ok) : TC(warn);
        lv_draw_rect(ctx, &r, &s);
    }
}

/* The hour: the minutes with a marker green, now marked */
static void sched_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t     *ctx = lv_event_get_draw_ctx(e);
    lv_area_t          a, s;
    lv_draw_rect_dsc_t r;
    time_t             now = time(NULL);
    lv_coord_t         w;

    lv_obj_get_coords(sched, &a);
    w = a.x2 - a.x1;
    lv_draw_rect_dsc_init(&r);
    for (int m = 0; m < 60; m++) {
        s.x1 = a.x1 + m * w / 60;
        s.x2 = a.x1 + (m + 1) * w / 60 - 1;
        s.y1 = a.y1;
        s.y2 = a.y1 + 15;
        r.bg_color = clock_beacon_minute_ok(station, m) ? lv_color_mix(TC(ok), TC(row), 110) : TC(row);
        lv_draw_rect(ctx, &r, &s);
    }
    s.x1 = a.x1 + (lv_coord_t)((now % 3600) * w / 3600) - 1;
    s.x2 = s.x1 + 2;
    s.y1 = a.y1 - 4;
    s.y2 = a.y1 + 15;
    r.bg_color = TC(warn);
    lv_draw_rect(ctx, &r, &s);
}

static void station_view_update(void) {
    clock_beacon_state_t st;
    const clock_beacon_t *b = &clock_beacons[station];
    time_t               now = time(NULL);
    int                  next = clock_beacon_next_s(station, now), h = holder();
    char                 t[24], by[24];
    const char          *why;

    clock_beacon_get(&st);
    beacon_result(&st);
    tone_db = st.on ? st.tone_db : 0;

    {   /* "RWM 9996": the call bold, the frequency after it */
        const char *sp = strchr(b->name, ' ');
        int         n  = sp ? (int)(sp - b->name) : (int)strlen(b->name);

        lv_label_set_text_fmt(l_st_name, "%.*s", n, b->name);
        lv_label_set_text_fmt(l_st_freq, "%s kHz", sp ? sp + 1 : "");
        lv_obj_align_to(l_st_freq, l_st_name, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -2);
        lv_obj_update_layout(l_st_freq);
        lv_obj_align_to(chip, l_st_freq, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    }
    if (!st.on) {
        lv_label_set_text(l_chip, "OFF");
        lv_obj_set_style_text_color(l_chip, TC(text_mute), 0);
    } else if (next > 60) {
        lv_label_set_text_fmt(l_chip, "WAITS FOR :%02d", (int)((now + next) / 60 % 60));
        lv_obj_set_style_text_color(l_chip, TC(warn), 0);
    } else {
        lv_label_set_text(l_chip, "LISTENING");
        lv_obj_set_style_text_color(l_chip, TC(ok), 0);
    }
    if (clock_sync_kept(&why))
        lv_label_set_text_fmt(l_holder, "Clock kept by %s: learning the delay", why);
    else if (h == SRC_STATION && clock_sync_station(by, sizeof(by), &(time_t){0}, &(double){0}))
        lv_label_set_text_fmt(l_holder, "Clock set by %s", by);
    else if (h == SRC_FT8)
        lv_label_set_text(l_holder, "Clock set by FT8: a marker pair sets it better");
    else
        lv_label_set_text(l_holder, "Clock chip: a marker pair sets it");

    if (next < 0)
        lv_label_set_text(l_next, "-");
    else if (next < 120)
        lv_label_set_text_fmt(l_next, "%d s", next);
    else
        lv_label_set_text_fmt(l_next, "%d min", next / 60);
    lv_label_set_text_fmt(l_tone, st.on ? "%.0f dB" : "-", tone_db);
    lv_obj_invalidate(dial);
    lv_obj_invalidate(tone_bar);
    lv_obj_invalidate(sched);

    float delay = clock_sync_beacon_delay(NULL);

    for (int i = 0; i < CLOCK_BEACON_HIST; i++) {
        if (i >= st.hist_n || !st.on) {
            lv_obj_add_flag(hist_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const clock_beacon_marker_t *m = &st.hist[i];
        float                        ahead = -m->raw - delay;
        struct tm                    tm;

        gmtime_r(&m->at, &tm);
        lv_obj_clear_flag(hist_box[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(hist_t[i], "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        snprintf(t, sizeof(t), "%s %.3f s", ahead >= 0 ? "ahead" : "behind", fabsf(ahead));
        lv_label_set_text(hist_off[i], i == 0 && !m->agree ? "first" : t);
        lv_label_set_text_fmt(hist_db[i], "%.0f dB", m->snr_db);
        lv_label_set_text(hist_ok[i], m->agree ? "agree" : "");
    }
}

static void status_cb(lv_timer_t *t) {
    (void)t;
    if (station_view) {
        station_view_update();
    } else {
        clock_beacon_state_t st;

        clock_beacon_get(&st);
        beacon_result(&st);         /* listening goes on in the Clock view */
        clock_view_update();
    }
}

/* ---- building the views ---- */

static void build_clock_view(void) {
    static const char *names[SRC_N] = {"NTP (WiFi)", "GPS", "Time station", "FT8"};
    lv_obj_t          *v = view_clock = box(win, 0, 0, W, DIALOG_H - 8, TC(panel));

    lv_obj_set_size(v, lv_pct(100), lv_pct(100));

    lv_obj_set_style_radius(v, 0, 0);
    lv_obj_t *cap = label(v, &sony_18, TC(text_mute), 16, 4);
    lv_label_set_text(cap, "CLOCK");
    extern const lv_font_t arial_60;    /* F4OLI's choice: the boldest, read from afar */

    l_time = label(v, &arial_60, TC(text), 12, 18);
    l_date = label(v, &sony_20, TC(text_mute), 16, 66);

    keep_box      = box(v, 16, 100, 364, 96, TC(row));
    cap           = label(keep_box, &sony_16, TC(text_mute), 14, 6);
    lv_label_set_text(cap, "kept by");
    l_keep_name   = label(keep_box, &sony_30, TC(ok), 14, 24);
    l_keep_detail = label(keep_box, &sony_18, TC(text), 14, 66);
    lv_label_set_long_mode(l_keep_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l_keep_detail, 340);
    l_gps         = label(v, &sony_20, TC(text_mute), 16, 212);
    l_ft8         = label(v, &sony_20, TC(text_mute), 16, 242);
    lv_label_set_long_mode(l_gps, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(l_ft8, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l_gps, 370);
    lv_obj_set_width(l_ft8, 370);

    cap = label(v, &sony_18, TC(text_mute), LADDER_X, 6);
    lv_label_set_text(cap, "Who sets the clock (first to last)");
    for (int i = 0; i < SRC_N; i++) {
        lv_obj_t *b = src_box[i] = box(v, LADDER_X, 36 + i * 56, LADDER_W, 50, TC(row));
        lv_obj_t *n = label(b, &sony_22, TC(text_mute), 10, 12);

        lv_label_set_text_fmt(n, "%d", i + 1);
        src_name[i]   = label(b, &sony_22, TC(text), 36, 2);
        lv_label_set_text(src_name[i], names[i]);
        src_detail[i] = label(b, &sony_16, TC(text_mute), 36, 28);
        lv_label_set_long_mode(src_detail[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(src_detail[i], LADDER_W - 120);
        src_tag[i]    = label(b, &sony_18, TC(text_mute), 0, 0);
        lv_obj_align(src_tag[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

static void build_station_view(void) {
    lv_obj_t *v = view_station = box(win, 0, 0, W, DIALOG_H - 8, TC(panel));

    lv_obj_set_size(v, lv_pct(100), lv_pct(100));

    lv_obj_set_style_radius(v, 0, 0);
    extern const lv_font_t arial_28, arial_36;

    l_st_name = label(v, &arial_28, TC(text), 12, 4);      /* "RWM" bold, its frequency lighter after it */
    l_st_freq = label(v, &sony_24, TC(text_mute), 0, 0);
    chip      = box(v, 236, 4, 190, 32, TC(row));
    lv_obj_set_style_border_width(chip, 0, 0);
    l_chip    = label(chip, &sony_18, TC(ok), 0, 0);
    lv_obj_align(l_chip, LV_ALIGN_CENTER, 0, 0);
    l_holder  = label(v, &sony_18, TC(text_mute), 0, 0);
    lv_obj_align(l_holder, LV_ALIGN_TOP_RIGHT, -12, 10);

    dial = lv_obj_create(v);
    lv_obj_remove_style_all(dial);
    lv_obj_set_size(dial, 2 * DIAL_R + 8, 2 * DIAL_R + 8);
    lv_obj_set_pos(dial, 8, 56);
    lv_obj_clear_flag(dial, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dial, dial_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_t *nm = label(v, &sony_14, TC(text_mute), 0, 0);
    lv_obj_set_style_text_letter_space(nm, 2, 0);
    lv_label_set_text(nm, "NEXT MARKER");
    lv_obj_align_to(nm, dial, LV_ALIGN_CENTER, 0, 22);
    l_next = label(v, &arial_36, TC(warn), 0, 0);
    lv_obj_set_width(l_next, 2 * DIAL_R);
    lv_obj_set_style_text_align(l_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(l_next, dial, LV_ALIGN_CENTER, 0, 48);

    lv_obj_t *cap = label(v, &sony_18, TC(text_mute), HIST_X, 44);
    lv_label_set_text(cap, "Marker tone");
    tone_bar = lv_obj_create(v);
    lv_obj_remove_style_all(tone_bar);
    lv_obj_set_size(tone_bar, SEGS * 9, 22);
    lv_obj_set_pos(tone_bar, HIST_X, 68);
    lv_obj_add_event_cb(tone_bar, tone_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    l_tone = label(v, &sony_24, TC(text), HIST_X + SEGS * 9 + 14, 64);

    cap = label(v, &sony_18, TC(text_mute), HIST_X, 98);
    lv_label_set_text(cap, "Markers heard");
    for (int i = 0; i < CLOCK_BEACON_HIST; i++) {
        lv_obj_t *b = hist_box[i] = box(v, HIST_X, 122 + i * 29, HIST_W, 27, TC(row));

        lv_obj_set_style_border_width(b, 0, 0);
        hist_t[i]   = label(b, &sony_20, TC(text), 10, 2);
        hist_off[i] = label(b, &sony_20, TC(text), 120, 2);
        hist_db[i]  = label(b, &sony_20, TC(text_mute), 320, 2);
        hist_ok[i]  = label(b, &sony_20, TC(ok), 0, 0);
        lv_obj_align(hist_ok[i], LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    }

    cap = label(v, &sony_16, TC(text_mute), HIST_X, 238);
    lv_label_set_text(cap, "This hour: the minutes with a marker");
    sched = lv_obj_create(v);
    lv_obj_remove_style_all(sched);
    lv_obj_set_size(sched, HIST_W, 16);
    lv_obj_set_pos(sched, HIST_X, 260);
    lv_obj_add_event_cb(sched, sched_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    for (int m = 0; m <= 60; m += 10) {
        lv_obj_t *l = label(v, &sony_14, TC(text_mute), HIST_X + m * HIST_W / 60 - (m == 60 ? 24 : m ? 10 : 0), 276);
        lv_label_set_text_fmt(l, ":%02d", m);
    }
}

static void show_view(bool st) {
    station_view = st;
    if (st) {
        lv_obj_add_flag(view_clock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(view_station, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(view_station, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(view_clock, LV_OBJ_FLAG_HIDDEN);
    }
    buttons_refresh(&btn_view);
    status_cb(NULL);
}

/* ---- buttons ---- */

static const char *view_label(void) {
    return station_view ? "View:\nStation" : "View:\nClock";
}

static const char *station_label(void) {
    static char text[32];

    snprintf(text, sizeof(text), "Station:\n%s", clock_beacons[station].name);
    return text;
}

static const char *listen_label(void) {
    return dialog.audio_cb ? "Stop\nlistening" : "Listen";
}

static const char *ft8_auto_label(void) {
    return param_i_get(cfg_clock_ft8_auto) ? "FT8 Auto:\nOn" : "FT8 Auto:\nOff";
}

/* The station's marker as a tone in USB; frequency and mode back when the window closes */
static void tune(void) {
    const clock_beacon_t *b  = &clock_beacons[station];
    freq_list_item_t      it = {.freq_hz = b->freq_hz, .name = b->name};

    if (!tuned) {
        dialog_restore_radio_on_close();
        tuned = true;
    }
    freq_list_tune(&it, b->freq_hz - b->dial_hz);
}

static void listen_start(void) {
    tune();
    clock_beacon_start(station);
    dialog.audio_cb = clock_beacon_audio;
}

static void listen_stop(void) {
    dialog.audio_cb = NULL;
    clock_beacon_stop();
}

static void view_cb(button_data_t *btn) {
    (void)btn;
    show_view(!station_view);
}

static void station_cb(button_data_t *btn) {
    (void)btn;
    station = (station + 1) % clock_beacons_n;
    buttons_refresh(&btn_station);
    if (dialog.audio_cb)
        listen_start();
    status_cb(NULL);
}

static void listen_cb(button_data_t *btn) {
    (void)btn;
    if (dialog.audio_cb) {
        listen_stop();
    } else {
        listen_start();
        if (!station_view)
            show_view(true);    /* listening: the station's view */
    }
    buttons_refresh(&btn_listen);
    status_cb(NULL);
}

static void ft8_set_cb(button_data_t *btn) {
    char msg[96];

    (void)btn;
    clock_sync_ft8_apply(msg, sizeof(msg));
    msg_update_text_fmt("%s", msg);
}

static void ft8_auto_cb(button_data_t *btn) {
    (void)btn;
    param_i_set(cfg_clock_ft8_auto, !param_i_get(cfg_clock_ft8_auto));
    buttons_refresh(&btn_ft8_auto);
    msg_update_text_fmt(param_i_get(cfg_clock_ft8_auto)
                            ? "FT8 sets the clock in the field (no NTP, no GPS, no time station since the start)"
                            : "FT8 only measures the clock: Set from FT8 steps it");
}

static void open_cb(lv_obj_t *parent) {
    win        = dialog_init(parent);
    dialog.obj = win;
    tuned      = false;
    gps_text[0] = '\0';
    lv_obj_clear_flag(win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(win, LV_SCROLLBAR_MODE_OFF);
    build_clock_view();
    build_station_view();

    lv_obj_add_event_cb(win, report_cb, EVENT_GPS, NULL);
    lv_obj_add_event_cb(win, window_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(keyboard_group, win);

    /* ten times a second: the dial's hand, the tone's level */
    status_timer = lv_timer_create(status_cb, 100, NULL);
    show_view(false);
}

static void close_cb(void) {
    listen_stop();
    lv_timer_del(status_timer);
    status_timer = NULL;
}

static void window_key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    if (key == LV_KEY_ESC)
        dialog_destruct();
    else if (key == KEY_VOL_LEFT_EDIT || key == KEY_VOL_LEFT_SELECT)
        radio_change_vol(-1);
    else if (key == KEY_VOL_RIGHT_EDIT || key == KEY_VOL_RIGHT_SELECT)
        radio_change_vol(1);
}
