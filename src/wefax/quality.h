/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax: a quality mark (0-100) of a received fax,
 *  the v2 scale of the station's qualite.py (02/11/2026), so that Auto Plan
 *  keeps the best copy of each chart.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned lines;
    float    coherence;     /* a dark trace goes on into the next row, noise does not */
    float    fading;        /* rows whose mean is off the local norm */
    float    glissement;    /* rows shifted > 3 px from the previous one (lost sync) */
    float    isoles;        /* dark pixels with no dark neighbour (crackle) */
    float    blancs;        /* clean white 16 x 16 blocks */
    float    fond;          /* white level: median of the rows' 90th percentile */
    float    contraste;     /* white minus the median of the dark pixels */
    float    encre;         /* dark pixels (< 110) */
    float    stries;        /* rows whose background jumps > 4 levels (horizontal stripes) */
} wefax_quality_measures_t;

/* The mark of a fax, rows of width pixels (grey, 0 = black). no_stop: the stop tone was not heard (the
 * footer is probably cut), x 0.8. Slow (~0.5 s on the radio): a worker thread. m may be NULL. */
float wefax_quality(const uint8_t *img, unsigned width, unsigned rows, bool no_stop, wefax_quality_measures_t *m);

#ifdef __cplusplus
}
#endif
