/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax charts at night.
 *
 *  The sun's elevation from the usual low-accuracy formulas (declination and equation of time by the day of the
 *  year), a degree or so: plenty to tell day from night.
 */

#include "wefax_night.h"

#include "cfg/cfg_api.h"
#include "qth/qth.h"
#include "station.h"

#include <math.h>
#include <time.h>

static double sun_elevation(double lat, double lon, time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);

    double g    = 2 * M_PI / 365.0 * (tm.tm_yday + (tm.tm_hour - 12) / 24.0);
    double decl = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g) -
                  0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
    double eqt  = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) - 0.014615 * cos(2 * g) -
                           0.040849 * sin(2 * g));                                   /* minutes */
    double tst  = tm.tm_hour * 60 + tm.tm_min + tm.tm_sec / 60.0 + eqt + 4 * lon;    /* true solar time */
    double ha   = (tst / 4 - 180) * M_PI / 180;
    double la   = lat * M_PI / 180;
    double s    = sin(la) * sin(decl) + cos(la) * cos(decl) * cos(ha);

    return asin(s) * 180 / M_PI;
}

bool wefax_night(void)
{
    static time_t asked;
    static bool   night;
    int           mode = (int)param_i_get(cfg_wefax_night);
    time_t        now  = time(NULL);

    if (mode == 1)
        return true;
    if (mode == 2)
        return false;
    if (now - asked < 60)
        return night;
    asked = now;

    const char *qth = station_qth();
    double      lat, lon;

    if (!qth || !qth[0] || now < 1700000000)   /* no locator, or a clock not set: never dim by guess */
        return night = false;
    qth_str_to_pos(qth, &lat, &lon);
    return night = sun_elevation(lat, lon, now) < -6.0;
}

const char *wefax_night_label(void)
{
    switch (param_i_get(cfg_wefax_night)) {
        case 1:  return "Night\nOn";
        case 2:  return "Night\nOff";
        default: return wefax_night() ? "Night\nAuto (on)" : "Night\nAuto";
    }
}

void wefax_night_next(void)
{
    param_i_set(cfg_wefax_night, (param_i_get(cfg_wefax_night) + 1) % 3);
}
