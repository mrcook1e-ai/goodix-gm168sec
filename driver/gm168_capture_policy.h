/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * gm168_capture_policy.h — pure capture-selection policy for the GM168
 * quality gate: conservative dual-frame picking and a pre-stretch raw
 * ranking score.
 *
 * Kept free of GLib, libfprint, and libm so the standalone contract test
 * (tests/test_timeouts.c) exercises the exact helpers the driver uses, with
 * deterministic synthetic buffers only.
 *
 * Invariants:
 *   - Raw ranks <= 0 mean "no rank" (flat, NULL, or saturated input), not
 *     automatic rejection; post-stretch quality independently gates usability.
 *   - The raw rank is an UNCALIBRATED relative ordering key; it must never
 *     be compared against GM168_QUALITY_THRESH (that gate stays on the
 *     post-stretch gm168_quality_metric in goodix_gm168.c).
 *   - No sqrt / libm calls: variance is returned as-is.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* ---- Background capture gate ------------------------------------------- */

/* Provisional minimum mean 12-bit level of an averaged dark frame accepted
 * as the background reference. Retuned 2200 -> 2500 after owner-observed
 * partial-contact backgrounds passed the old gate on one device. This is not
 * a cross-device calibration. Override via $GM168_BG_MIN_MEAN when validating
 * other hardware (see gm168_bg_min_mean()). */
#define GM168_BG_MIN_MEAN_DEFAULT 2500

/* ---- Dual-capture selection --------------------------------------------- */

/*
 * Pick between dual frame #1 and frame #2. Post-stretch quality determines
 * usability; the pre-stretch raw rank only orders two usable frames.
 * Conservative: the settled second frame wins unless the first clearly beats
 * it.
 *
 *   q2 <= 0               -> 1  (second unusable: take first)
 *   q1 <= 0 && q2 > 0     -> 2  (first unusable: take second)
 *   rank2 >= 0.80 * rank1 -> 2  (includes ties: settled frame wins)
 *   otherwise             -> 1  (clear first-frame win)
 *
 * NaN quality is treated as unusable. A zero raw rank is valid for a usable
 * frame and ties another zero rank; it must not be confused with q == 0.
 * The returned value is only 1 or 2 and implies no ownership.
 */
static inline int
gm168_dual_pick (float q1, float q2, float rank1, float rank2)
{
    if (!(q2 > 0.0f))
        return 1;
    if (!(q1 > 0.0f))
        return 2;
    if (!(rank1 >= 0.0f))
        rank1 = 0.0f;
    if (!(rank2 >= 0.0f))
        rank2 = 0.0f;
    /* Express the 0.80 margin as 4/5 to avoid a rounded 0.80f constant.
     * At the representable 100/80 test boundary the second frame wins. */
    if (rank2 * 5.0f >= rank1 * 4.0f)
        return 2;
    return 1;
}

/* A high raw variance must not rescue a frame rejected by the post-stretch
 * saturation/quality gate. The driver keeps such frames only for the existing
 * all-zero fallback, with an effective selection rank of zero. */
static inline float
gm168_effective_rank (float post_quality, float raw_rank)
{
    return (post_quality > 0.0f && raw_rank > 0.0f) ? raw_rank : 0.0f;
}

/* ---- Pre-stretch raw rank ----------------------------------------------- */

/* Raw 12-bit saturation limits. Values clamped at either rail by the ADC
 * carry no ridge information. */
#define GM168_RAW_SAT_LOW   0x0000u
#define GM168_RAW_SAT_HIGH  0x0fffu

/* Fraction of central-crop pixels allowed to sit on a saturation rail
 * before the whole frame is rejected (mirrors the 30% post-stretch gate in
 * gm168_quality_metric()). */
#define GM168_RAW_RANK_SAT_LIMIT 0.30f

/*
 * Relative ranking score of a freshly decoded 12-bit raw frame, computed
 * BEFORE the envelope stretch so the aggressive per-pixel normalisation
 * cannot bias the comparison. Used only to order two usable candidates in
 * the dual-capture success and error-fallback paths.
 *
 * Score = population variance of the background-subtracted signal over the
 * central crop (x: [width/5, width - width/5), y: [height/8, height -
 * height/8)) — ridge structure shows up as high variance, flat and
 * saturated frames as 0. `bg` may be NULL (early init before the BG loop
 * finishes); the raw values are then ranked directly, which is noisy but
 * still monotonic with real ridge content. `raw` NULL, degenerate geometry,
 * or a saturated crop returns 0.0f.
 *
 * The result is always finite (12-bit bounded inputs, double accumulation)
 * and never compared against GM168_QUALITY_THRESH.
 */
static inline float
gm168_raw_rank (const uint16_t *raw,
                const uint16_t *bg,
                int             width,
                int             height)
{
    int x0, x1, y0, y1, n, sat = 0;
    double sum = 0.0, sq = 0.0, mean, var;
    int y, x;

    if (!raw || width <= 0 || height <= 0)
        return 0.0f;

    x0 = width / 5;
    x1 = width - width / 5;
    y0 = height / 8;
    y1 = height - height / 8;
    n = (x1 - x0) * (y1 - y0);
    if (n <= 0)
        return 0.0f;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            size_t i = (size_t) y * (size_t) width + (size_t) x;
            uint16_t v = raw[i];
            double d;

            if (v <= GM168_RAW_SAT_LOW || v >= GM168_RAW_SAT_HIGH)
                sat++;
            d = bg ? (double) ((int) v - (int) bg[i]) : (double) v;
            sum += d;
            sq += d * d;
        }
    }

    if ((float) sat / (float) n > GM168_RAW_RANK_SAT_LIMIT)
        return 0.0f;

    mean = sum / (double) n;
    var = sq / (double) n - mean * mean;
    /* var <= 0 covers flat frames; !(var > 0) also catches a defensive NaN. */
    if (!(var > 0.0))
        return 0.0f;
    return (float) var;
}
