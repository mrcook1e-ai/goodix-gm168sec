/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Goodix GM168SEC cancellation lifecycle policy.
 *
 * Kept free of USB and libfprint types so the standalone timeout/lifecycle
 * contract test exercises the exact predicate used by the driver.
 */
#pragma once

/* A client may cancel immediately after an early verify-match while the
 * post-capture REARM is still restoring FDT. Preserve that SSM only while it
 * can make progress; capture/recovery and a physically removed device still
 * take the immediate cancellation path. */
static inline int
gm168_should_drain_rearm (int rearm_active, int device_dead)
{
    return rearm_active && !device_dead;
}
