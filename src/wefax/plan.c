/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - WeFax Auto Plan, the slots.
 */

#include "plan.h"
#include "plan_default.h"

#include "../board.h"
#include "../cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAN_FILE   BOARD_DATA "/wefax/plan.json"

static wefax_slot_t slots[WEFAX_PLAN_MAX];
static int          slots_n;
static bool         from_file;

static int by_start(const void *a, const void *b)
{
    return (int)((const wefax_slot_t *)a)->min - (int)((const wefax_slot_t *)b)->min;
}

/* {"slots": [{"t": "HH:MM", "st", "hz", "prod", "dur", "fill"}, ...]}; the number read, -1 if it is not a plan */
static int parse(const char *text)
{
    cJSON *json = cJSON_Parse(text);
    cJSON *list = json ? cJSON_GetObjectItem(json, "slots") : NULL;
    int    n    = 0;

    if (!cJSON_IsArray(list)) {
        cJSON_Delete(json);
        return -1;
    }

    cJSON *it;

    cJSON_ArrayForEach(it, list) {
        cJSON *t = cJSON_GetObjectItem(it, "t"), *st = cJSON_GetObjectItem(it, "st");
        cJSON *hz = cJSON_GetObjectItem(it, "hz"), *prod = cJSON_GetObjectItem(it, "prod");
        cJSON *dur = cJSON_GetObjectItem(it, "dur"), *fill = cJSON_GetObjectItem(it, "fill");
        int    h, m;

        if (n == WEFAX_PLAN_MAX || !cJSON_IsString(t) || sscanf(t->valuestring, "%d:%d", &h, &m) != 2 || h < 0 ||
            h > 23 || m < 0 || m > 59 || !cJSON_IsNumber(hz) || hz->valuedouble < 100000 || hz->valuedouble > 30000000)
            continue;

        wefax_slot_t *s = &slots[n++];

        memset(s, 0, sizeof(*s));
        s->min     = (uint16_t)(h * 60 + m);
        s->freq_hz = (int32_t)hz->valuedouble;
        s->dur     = cJSON_IsNumber(dur) && dur->valuedouble >= 60 && dur->valuedouble <= 3600 ? (uint16_t)dur->valuedouble
                                                                                                : 690;
        s->fill    = cJSON_IsTrue(fill);
        snprintf(s->station, sizeof(s->station), "%s", cJSON_IsString(st) ? st->valuestring : "");
        snprintf(s->product, sizeof(s->product), "%s", cJSON_IsString(prod) ? prod->valuestring : "");
    }
    cJSON_Delete(json);
    qsort(slots, n, sizeof(slots[0]), by_start);
    return n;
}

int wefax_plan_load(void)
{
    FILE *f = fopen(PLAN_FILE, "r");

    from_file = false;
    if (f) {
        char  *text = NULL;
        long   size;

        if (!fseek(f, 0, SEEK_END) && (size = ftell(f)) > 0 && size < 256 * 1024 && !fseek(f, 0, SEEK_SET) &&
            (text = calloc(1, size + 1)) && fread(text, 1, size, f) == (size_t)size) {
            int n = parse(text);

            if (n > 0) {
                slots_n   = n;
                from_file = true;
            } else {
                printf("WEFAX PLAN: %s is not a plan, the firmware's is used\n", PLAN_FILE);
            }
        }
        free(text);
        fclose(f);
    }
    if (!from_file)
        slots_n = parse(plan_default_json);
    if (slots_n < 0)
        slots_n = 0;
    printf("WEFAX PLAN: %d slots (%s)\n", slots_n, from_file ? PLAN_FILE : "firmware");
    return slots_n;
}

int wefax_plan_count(void)
{
    return slots_n;
}

const wefax_slot_t *wefax_plan_slot(int i)
{
    return i >= 0 && i < slots_n ? &slots[i] : NULL;
}

bool wefax_plan_from_file(void)
{
    return from_file;
}

bool wefax_plan_product_unique(const char *product)
{
    return !product[0] || strstr(product, "inconnu");
}
