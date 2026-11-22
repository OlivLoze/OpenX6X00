/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - the clock in the field from FT8 and from the time stations.
 *
 *  FT8: most stations heard keep their clock from the internet or a GPS, so where their decodes start in our slots
 *  says where our clock is. Not to the absolute: the radio's audio path and the stations' own habits move every
 *  decode alike. So while NTP keeps our clock (at home, WiFi) the band's usual start is learned (ref), and in the
 *  field the median of the decodes minus ref is how far our clock is off. Kept in BOARD_DATA/clock_sync.conf with the
 *  time stations' delay, learned the same way.
 */

#include "clock_sync.h"

#include "board.h"
#include "clock.h"
#include "gps.h"
#include "msg_box.h"
#include "cfg/cfg_api.h"
#include "ft8/utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONF_FILE       BOARD_DATA "/clock_sync.conf"

#define FT8_REF_DEFAULT 0.5f    /* WSJT-X keys 0.5 s into the slot */
#define FT8_SLOT_MAX    48      /* decodes kept a slot */
#define FT8_SLOTS       4       /* the estimate's slots: a minute of FT8 */
#define FT8_MIN_N       8       /* decodes for an estimate */
#define FT8_MAX_SPREAD  0.3f    /* their median distance to the median, s: over it the band does not agree */
#define FT8_LEARN_N     4       /* decodes in a slot to learn from */
#define FT8_LEARN_OFF   0.3f    /* a slot this far from what was learned is left out */
#define FT8_STEP_MIN    0.2f    /* closer than this the clock is right enough for FT8 and JS8 */
#define FT8_AUTO_GAP_S  120     /* between two Auto steps */
#define FT8_AGE_MAX_S   900     /* the oldest estimate Set from FT8 takes */

#define BEACON_DELAY_DEFAULT 0.04f
#define SAVE_GAP_S      120     /* the learned values to the card at most this often */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static char     last_by[24];
static char     station_by[24];     /* a time station set the clock since the start: FT8 leaves it alone */
static time_t   last_when;
static double   last_step;
static unsigned generation;

/* FT8: the slot going on, the slots before it */
static float    cur[FT8_SLOT_MAX];
static int      cur_n;
static bool     cur_skip;       /* the clock stepped in this slot: its decodes are off */
static float    past[FT8_SLOTS][FT8_SLOT_MAX];
static int      past_n[FT8_SLOTS];
static int      past_i;
static time_t   auto_last;

static clock_ft8_t est;
static float    ft8_ref = FT8_REF_DEFAULT;
static int      ft8_ref_slots;

static float    beacon_delay = BEACON_DELAY_DEFAULT;
static int      beacon_learned;

static bool     loaded;
static time_t   saved_at;
static float    saved_ref = NAN, saved_delay = NAN;

static void load(void) {
    FILE *f;
    char  line[64];

    if (loaded)
        return;
    loaded = true;
    if (!(f = fopen(CONF_FILE, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        float v;
        int   n;

        if (sscanf(line, "ft8_ref %f %d", &v, &n) == 2 && v > -1.0f && v < 3.0f) {
            ft8_ref       = v;
            ft8_ref_slots = n;
        } else if (sscanf(line, "beacon_delay %f %d", &v, &n) == 2 && v > -0.5f && v < 1.0f) {
            beacon_delay   = v;
            beacon_learned = n;
        }
    }
    fclose(f);
    saved_ref   = ft8_ref;
    saved_delay = beacon_delay;
}

/* Under lock; the card spared: a change of 5 ms or more, ten minutes apart */
static void save(void) {
    time_t now = time(NULL);
    FILE  *f;

    if (now - saved_at < SAVE_GAP_S)
        return;
    if (fabsf(ft8_ref - saved_ref) < 0.005f && fabsf(beacon_delay - saved_delay) < 0.005f)
        return;
    saved_at = now;
    if (!(f = fopen(CONF_FILE ".tmp", "w")))
        return;
    fprintf(f, "ft8_ref %.4f %d\nbeacon_delay %.4f %d\n", ft8_ref, ft8_ref_slots, beacon_delay, beacon_learned);
    if (fclose(f) == 0 && rename(CONF_FILE ".tmp", CONF_FILE) == 0) {
        saved_ref   = ft8_ref;
        saved_delay = beacon_delay;
    }
}

/* ntpd's own word on the clock (ntpq -c rv): the kernel's STA_UNSYNC is no use here, a step (ours, ntpd's) sets it and
 * ntpd clears it only at its next poll, up to 17 min on. A mode 6 read of the system variables to 127.0.0.1: the
 * reply's status word has the leap bits (3: not synchronised) and the sync source (5: its local clock, not a source) */
static bool ntpd_synced(void) {
    static pthread_mutex_t ask = PTHREAD_MUTEX_INITIALIZER;
    static time_t          asked;
    static bool            synced;
    time_t                 now = time(NULL);

    pthread_mutex_lock(&ask);
    if (now - asked < 10) {
        bool s = synced;

        pthread_mutex_unlock(&ask);
        return s;
    }
    asked  = now;
    synced = false;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd >= 0) {
        struct sockaddr_in to  = {.sin_family = AF_INET, .sin_port = htons(123)};
        uint8_t            q[12] = {0x16, 2, 0, 1};      /* LI 0, version 2, mode 6; READVAR, sequence 1, association 0 */
        uint8_t            r[512];
        struct pollfd      pfd = {.fd = fd, .events = POLLIN};

        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (sendto(fd, q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to)) == sizeof(q) && poll(&pfd, 1, 200) == 1 &&
            recv(fd, r, sizeof(r), 0) >= 12 && (r[0] & 7) == 6 && (r[1] & 0x80)) {
            int leap   = r[4] >> 6;
            int source = r[4] & 0x3f;

            synced = leap != 3 && source != 0 && source != 5;
        }
        close(fd);
    }

    bool s = synced;

    pthread_mutex_unlock(&ask);
    return s;
}

bool clock_sync_kept(const char **why) {
    double lat, lon;
    int    age;

    /* NTP first: ntpd prefers the internet's servers to the GPS (stratum 4 in ntp.conf) */
    if (ntpd_synced() || ftx_clock_synced(NULL)) {
        if (why)
            *why = "NTP";
        return true;
    }
    if (gps_last_fix(&lat, &lon, &age) && age <= 10) {
        if (why)
            *why = "GPS";
        return true;
    }
    return false;
}

#define START_CHECK_MS (2 * 60 * 1000)

static void start_check_cb(lv_timer_t *t) {
    (void)t;
    if (clock_sync_kept(NULL) || clock_sync_generation() != 0)
        return;
    msg_update_text_fmt("Clock not set since the start (no WiFi, no GPS): APP > GPS / Clock > Listen, "
                        "or FT8 with Auto Sync on");
}

void clock_sync_start_check(void) {
    lv_timer_set_repeat_count(lv_timer_create(start_check_cb, START_CHECK_MS, NULL), 1);
}

bool clock_sync_ft8_held(const char **why) {
    static char by[24];

    if (clock_sync_kept(why))
        return true;
    pthread_mutex_lock(&lock);
    snprintf(by, sizeof(by), "%s", station_by);
    pthread_mutex_unlock(&lock);
    if (!by[0])
        return false;
    if (why)
        *why = by;
    return true;
}

bool clock_sync_step(double step, const char *by) {
    struct timespec now;
    int64_t         ns;

    clock_gettime(CLOCK_REALTIME, &now);
    ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + (int64_t)llround(step * 1e9);

    struct timespec to = {.tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL};

    if (clock_settime(CLOCK_REALTIME, &to) != 0) {
        LV_LOG_ERROR("Can't set system time: %s", strerror(errno));
        return false;
    }
    clock_rtc_save();       /* kept across a power-off */

    pthread_mutex_lock(&lock);
    snprintf(last_by, sizeof(last_by), "%s", by);
    if (strcmp(by, "FT8"))     /* a time station: it holds against FT8 until the radio stops */
        snprintf(station_by, sizeof(station_by), "%s", by);
    last_when = to.tv_sec;
    last_step = step;
    generation++;
    /* every FT8 measure so far was taken on the clock before */
    cur_n    = 0;
    cur_skip = true;
    memset(past_n, 0, sizeof(past_n));
    est.valid = false;
    pthread_mutex_unlock(&lock);
    printf("CLOCK: %+.3f s from %s\n", step, by);
    return true;
}

bool clock_sync_last(char *by, size_t size, time_t *when, double *step) {
    pthread_mutex_lock(&lock);
    bool any = last_when != 0;

    snprintf(by, size, "%s", last_by);
    *when = last_when;
    *step = last_step;
    pthread_mutex_unlock(&lock);
    return any;
}

unsigned clock_sync_generation(void) {
    pthread_mutex_lock(&lock);
    unsigned g = generation;
    pthread_mutex_unlock(&lock);
    return g;
}

/* ---- FT8 ---- */

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

static float median(float *v, int n) {
    qsort(v, n, sizeof(*v), cmp_float);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

void clock_sync_ft8_add(float t) {
    if (!isfinite(t))
        return;
    pthread_mutex_lock(&lock);
    if (cur_n < FT8_SLOT_MAX)
        cur[cur_n++] = t;
    pthread_mutex_unlock(&lock);
}

/* Under lock: the decodes of the last slots, where they say the clock is */
static void estimate(void) {
    float all[FT8_SLOTS * FT8_SLOT_MAX], dev[FT8_SLOTS * FT8_SLOT_MAX];
    int   n = 0, slots = 0;

    for (int s = 0; s < FT8_SLOTS; s++) {
        if (past_n[s])
            slots++;
        for (int i = 0; i < past_n[s]; i++)
            all[n++] = past[s][i];
    }
    if (n < FT8_MIN_N)
        return;

    float m = median(all, n);

    for (int i = 0; i < n; i++)
        dev[i] = fabsf(all[i] - m);
    if (median(dev, n) > FT8_MAX_SPREAD)
        return;
    est.valid = true;
    est.ahead = m - ft8_ref;
    est.n     = n;
    est.slots = slots;
    est.when  = time(NULL);
}

void clock_sync_ft8_slot_end(void) {
    bool kept = clock_sync_kept(NULL);

    pthread_mutex_lock(&lock);
    load();
    if (cur_skip || cur_n == 0) {
        cur_skip = false;
        cur_n    = 0;
        pthread_mutex_unlock(&lock);
        return;
    }
    if (kept) {
        /* the band's start with a right clock: the first slots straight in, then slowly */
        if (cur_n >= FT8_LEARN_N) {
            float v[FT8_SLOT_MAX];

            memcpy(v, cur, cur_n * sizeof(float));

            float m = median(v, cur_n);

            /* once learned, a slot far off is the band's odd minute (stations off together), not the radio */
            if (ft8_ref_slots >= 10 && fabsf(m - ft8_ref) > FT8_LEARN_OFF)
                goto past;
            ft8_ref = ft8_ref_slots < 5 ? (ft8_ref * ft8_ref_slots + m) / (ft8_ref_slots + 1)
                                        : ft8_ref + 0.05f * (m - ft8_ref);
            ft8_ref_slots++;
            save();
        }
    }
past:
    past_i = (past_i + 1) % FT8_SLOTS;
    memcpy(past[past_i], cur, cur_n * sizeof(float));
    past_n[past_i] = cur_n;
    cur_n          = 0;
    estimate();
    pthread_mutex_unlock(&lock);
}

void clock_sync_ft8_auto(float sec, float slot, bool sending) {
    time_t now = time(NULL);
    float  step;

    if (sending || slot < 7.0f || sec < 0.4f * slot || sec > 0.6f * slot || !param_i_get(cfg_clock_ft8_auto))
        return;
    pthread_mutex_lock(&lock);
    bool go = est.valid && est.slots >= 2 && now - est.when < (time_t)(2 * slot) && now - auto_last >= FT8_AUTO_GAP_S &&
              fabsf(est.ahead) >= FT8_STEP_MIN;

    step = -est.ahead;
    pthread_mutex_unlock(&lock);
    if (!go || clock_sync_ft8_held(NULL))
        return;
    auto_last = now;
    if (clock_sync_step(step, "FT8"))
        msg_schedule_text_fmt("Clock %+.2f s from the FT8 stations heard", step);
}

void clock_sync_ft8_get(clock_ft8_t *out) {
    pthread_mutex_lock(&lock);
    load();
    *out           = est;
    out->ref       = ft8_ref;
    out->ref_slots = ft8_ref_slots;
    pthread_mutex_unlock(&lock);
}

bool clock_sync_ft8_apply(char *msg, size_t size) {
    clock_ft8_t e;
    const char *why;

    clock_sync_ft8_get(&e);
    if (clock_sync_ft8_held(&why)) {
        snprintf(msg, size, "%s keeps the clock: nothing to set from FT8", why);
        return false;
    }
    if (!e.valid || time(NULL) - e.when > FT8_AGE_MAX_S) {
        snprintf(msg, size, "No FT8 measure: open FT8 a minute on a busy band");
        return false;
    }
    if (fabsf(e.ahead) < 0.05f) {
        snprintf(msg, size, "Clock right by FT8 (%+.2f s)", e.ahead);
        return false;
    }
    if (!clock_sync_step(-e.ahead, "FT8")) {
        snprintf(msg, size, "The clock can't be set");
        return false;
    }
    snprintf(msg, size, "Clock %+.2f s from %d FT8 decodes", -e.ahead, e.n);
    return true;
}

/* ---- time stations ---- */

float clock_sync_beacon_delay(int *learned) {
    pthread_mutex_lock(&lock);
    load();
    float d = beacon_delay;

    if (learned)
        *learned = beacon_learned;
    pthread_mutex_unlock(&lock);
    return d;
}

void clock_sync_beacon_learn(float delay) {
    if (!isfinite(delay) || delay < -0.5f || delay > 1.0f)
        return;
    pthread_mutex_lock(&lock);
    load();
    beacon_delay = beacon_learned ? beacon_delay + 0.3f * (delay - beacon_delay) : delay;
    beacon_learned++;
    save();
    pthread_mutex_unlock(&lock);
}
