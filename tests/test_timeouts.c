/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Standalone compile-time + runtime contract check for the GM168 timeout,
 * lifecycle, and capture-policy headers. Pure C11, no GLib, no -lm: all
 * inputs are deterministic synthetic buffers (no real frames, no device
 * constants beyond the layout macros under test).
 */
#include <stdio.h>
#include "gm168_lifecycle.h"
#include "gm168_timeouts.h"
#include "gm168_capture_policy.h"

_Static_assert (GM168_USB_RX_TIMEOUT_MS       == 2000, "RX default");
_Static_assert (GM168_USB_RX_SHORT_TIMEOUT_MS == 1000, "RX short");
_Static_assert (GM168_USB_RX_LONG_TIMEOUT_MS  == 3000, "RX long");
_Static_assert (GM168_USB_TX_TIMEOUT_MS       == 2000, "TX");
_Static_assert (GM168_WAKEUP_DELAY_MS         == 50,   "wakeup");
_Static_assert (GM168_TLS_PUMP_DELAY_MS       == 10,   "TLS pump");
_Static_assert (GM168_REARM_DELAY_MS          == 150,  "rearm");
_Static_assert (GM168_CAPTURE_SETTLE_MS       == 220,  "capture settle retuned 150 -> 220");
_Static_assert (GM168_WAIT_LIFT_POLL_MS       == 300,  "lift poll");
_Static_assert (GM168_WAIT_LIFT_MAX_REARMS    == 8,    "lift retries");
_Static_assert (GM168_REARM_DEADLINE_MS       == 6000, "rearm deadline");
_Static_assert (GM168_USB_RX_RETRY_LIMIT      == 60,   "RX retries");
_Static_assert (GM168_TLS_RETRY_LIMIT         == 60,   "TLS retries");

/* Background acceptance boundary: averaged dark frames below this mean are
 * rejected by the INIT BG loop. */
_Static_assert (GM168_BG_MIN_MEAN_DEFAULT == 2500, "BG min mean retuned 2200 -> 2500");

#define TEST_W 80
#define TEST_H 64

static int failures = 0;

static void
check (int cond, const char *what)
{
    if (!cond) {
        fprintf (stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void
test_dual_policy (void)
{
    /* Post-quality alone determines usability; raw ranks must not rescue a
     * rejected frame or hide a usable zero-rank frame. */
    check (gm168_dual_pick (50.0f, 0.0f, 100.0f, 0.0f) == 1,
           "dual: q2<=0 -> #1");
    check (gm168_dual_pick (50.0f, -1.0f, 100.0f, 0.0f) == 1,
           "dual: q2<0 -> #1");
    check (gm168_dual_pick (0.0f, 40.0f, 0.0f, 0.0f) == 2,
           "dual: usable zero-rank #2 beats rejected #1");
    check (gm168_dual_pick (40.0f, 0.0f, 0.0f, 250000.0f) == 1,
           "dual: rejected high-rank #2 cannot beat usable #1");
    check (gm168_dual_pick (0.0f, 0.0f, 0.0f, 0.0f) == 1,
           "dual: both zero -> #1");

    /* Equal raw ranks, including two usable zero ranks, prefer settled #2. */
    check (gm168_dual_pick (50.0f, 50.0f, 100.0f, 100.0f) == 2,
           "dual: rank tie -> #2");
    check (gm168_dual_pick (50.0f, 40.0f, 0.0f, 0.0f) == 2,
           "dual: usable zero-rank tie -> #2");

    /* 0.80 rank margin: at/above -> #2, clearly below -> #1. */
    check (gm168_dual_pick (50.0f, 50.0f, 100.0f, 80.0f) == 2,
           "dual: rank2 == 0.80*rank1 -> #2");
    check (gm168_dual_pick (50.0f, 50.0f, 100.0f, 81.0f) == 2,
           "dual: rank2 > margin -> #2");
    check (gm168_dual_pick (50.0f, 50.0f, 100.0f, 79.0f) == 1,
           "dual: rank2 just below margin -> #1");
    check (gm168_dual_pick (50.0f, 50.0f, 100.0f, 10.0f) == 1,
           "dual: clear #1 rank win");
    check (gm168_dual_pick (50.0f, 50.0f, 10.0f, 100.0f) == 2,
           "dual: clear #2 rank win");

    /* NaN quality is unusable; NaN ranks degrade to a zero-rank tie. */
    check (gm168_dual_pick (0.0f / 0.0f, 40.0f, 100.0f, 0.0f) == 2,
           "dual: NaN q1 -> #2");
    check (gm168_dual_pick (40.0f, 0.0f / 0.0f, 0.0f, 100.0f) == 1,
           "dual: NaN q2 -> #1");
    check (gm168_dual_pick (40.0f, 40.0f, 0.0f / 0.0f, 0.0f / 0.0f) == 2,
           "dual: NaN ranks -> zero-rank tie");

    /* A rejected post-stretch frame cannot outrank a usable candidate. */
    check (gm168_effective_rank (0.0f, 250000.0f) == 0.0f,
           "rank: post-quality zero suppresses raw rank");
    check (gm168_effective_rank (80.0f, 0.0f) == 0.0f,
           "rank: raw zero has no ordering rank");
    check (gm168_effective_rank (80.0f, 250000.0f) == 250000.0f,
           "rank: usable frame preserves raw rank");
}

static void
fill_const (unsigned short *buf, unsigned short v)
{
    for (int i = 0; i < TEST_W * TEST_H; i++)
        buf[i] = v;
}

static void
test_raw_rank (void)
{
    static unsigned short raw[TEST_W * TEST_H];
    static unsigned short bg[TEST_W * TEST_H];
    float flat, ridge, sat, nobg, degenerate;

    fill_const (bg, 2200);

    /* NULL input */
    check (gm168_raw_rank (NULL, bg, TEST_W, TEST_H) == 0.0f,
           "rank: NULL raw -> 0");

    /* flat raw == bg: no ridge content, must rank below any structured frame */
    fill_const (raw, 2200);
    flat = gm168_raw_rank (raw, bg, TEST_W, TEST_H);
    check (flat == 0.0f, "rank: flat raw==bg -> 0");

    /* ridge-like: alternating +/- 500 around the bg in the central crop.
     * Population variance of a symmetric two-level signal is 500^2. */
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++)
            raw[y * TEST_W + x] =
                (unsigned short) (2200 + ((x / 4) % 2 ? 500 : -500));
    ridge = gm168_raw_rank (raw, bg, TEST_W, TEST_H);
    check (ridge == 250000.0f, "rank: ridge-like raw variance == 500^2");
    check (flat < ridge, "rank: flat raw==bg below ridge-like");

    /* saturated raw frame rejected regardless of structure */
    fill_const (raw, 0x0fff);
    sat = gm168_raw_rank (raw, bg, TEST_W, TEST_H);
    check (sat == 0.0f, "rank: saturated raw -> 0");
    fill_const (raw, 0x0000);
    check (gm168_raw_rank (raw, bg, TEST_W, TEST_H) == 0.0f,
           "rank: zero-clamped raw -> 0");

    /* saturated central crop with structured surroundings still rejected */
    fill_const (raw, 2200);
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++)
            if (x >= 16 && x < 64 && y >= 8 && y < 56)
                raw[y * TEST_W + x] = 0x0fff;
    check (gm168_raw_rank (raw, bg, TEST_W, TEST_H) == 0.0f,
           "rank: saturated central crop -> 0");

    /* bg == NULL: ranked on raw directly, still monotonic */
    fill_const (raw, 2200);
    nobg = gm168_raw_rank (raw, NULL, TEST_W, TEST_H);
    check (nobg == 0.0f, "rank: flat raw without bg -> 0");
    for (int y = 0; y < TEST_H; y++)
        for (int x = 0; x < TEST_W; x++)
            raw[y * TEST_W + x] =
                (unsigned short) (2200 + ((x / 4) % 2 ? 500 : -500));
    check (gm168_raw_rank (raw, NULL, TEST_W, TEST_H) > nobg,
           "rank: ridge-like without bg above flat");

    /* finite metric on every path: !(v > 0) rejects NaN as well */
    fill_const (raw, 2200);
    degenerate = gm168_raw_rank (raw, bg, 0, TEST_H);
    check (degenerate == 0.0f, "rank: degenerate width -> 0");
    check (gm168_raw_rank (raw, bg, TEST_W, TEST_H) >= 0.0f,
           "rank: result finite");
}

int
main (void)
{
    /* Only a live REARM with the device still present may delay cancellation.
     * This exact predicate gates both io_cancellable and deferred DEINIT. */
    if (!gm168_should_drain_rearm (1, 0) ||
        gm168_should_drain_rearm (0, 0) ||
        gm168_should_drain_rearm (1, 1) ||
        gm168_should_drain_rearm (0, 1))
        return 1;

    test_dual_policy ();
    test_raw_rank ();

    if (failures != 0) {
        fprintf (stderr, "gm168 timeout/lifecycle/capture-policy contract: %d FAILURE(S)\n",
                 failures);
        return 1;
    }

    puts ("gm168 timeout/lifecycle/capture-policy contract: OK");
    return 0;
}
