# Changelog

All notable changes to the `goodix-gm168` overlay are documented in this file.

## [Unreleased]

### Added
- Immediate no-match completion in `fp_image_device_start_capture_action()`
  when `IDENTIFY` is called with a `NULL` or empty gallery, skipping sensor
  activation, state transitions, and SIGFM verify retries.
- Offline core tests `/sigfm/image_device/identify_empty_gallery`,
  `/sigfm/image_device/identify_match_before_finger_off`, and
  `/sigfm/image_device/enroll_fatal_during_scan` in `tests/test-fpi-sigfm.c`.
- Reciprocal SIGFM descriptor matching with a synthetic many-to-one regression
  test, plus internal forward-only/translation-consensus A/B controls.
  Translation consensus remains disabled in production.
- Normalized-frame SIGFM quality gating (`30` keypoints and `0.55` active `8x8`
  tile coverage by default) with centered-finger retries and deferred
  deactivation while finger presence or asynchronous extraction is active.
- `goodix-gm168-frame-layout` synthetic coverage of the exact production 12-bit
  unpack/transpose function, including all `5120` mappings and malformed input.
- Read-only recovery of the existing PSK when `psk.bin` is absent: fresh E4
  hash validation, dynamic DPAPI v1 parsing, exact white-box TLV validation,
  OpenSSL 3 decryption, constant-time hash verification, and in-memory TLS use.
  No recovered key or DPAPI area is persisted automatically.
- Dedicated synthetic `goodix-gm168-psk-recovery` unit tests for Python parity,
  malformed/truncated/overflow inputs, HMAC/PKCS#7/canonical-IV failures, hash
  mismatch, zero-PSK sentinels, bounded replies, and cleanup.

### Changed
- Overlapped usable-frame submission in `driver/goodix_gm168.c`:
  `capture_after_settle_cb()` no longer emits
  `fpi_image_device_report_finger_status(TRUE)`; usable frames (`quality > 0`)
  are submitted via `gm168_submit_capture()` before `start_rearm_ssm()` so SIGFM
  extraction and verify/identify matching can complete during `REARM`/`wait_lift`.
  `self->capture_submitted` remains `TRUE` until finger-off, and
  `dev_change_state(AWAIT_FINGER_OFF)` leaves reporting `finger_status(FALSE)`
  exclusively to `rearm_completed()` / `recover_completed()`.
  `GM168_CAPTURE_SETTLE_MS` (`150 ms`) and the `wait_lift` default (`150 ms`)
  are unchanged—responsiveness gains come solely from overlapping processing
  with `REARM`, with no new hardware timing or accuracy claims.
- Cancellation-safe `REARM` draining: an early-match client cancellation now
  preserves generation and `io_cancellable` while `rearm_active && !device_dead`,
  sets `deinit_pending`, and enters `DEINIT` only from `rearm_completed()` after
  success, error, or watchdog. Capture, `RECOVER`, and removal remain immediate.
  The shared predicate lives in `driver/gm168_lifecycle.h` and has a standalone
  four-case runtime contract in `tests/test_timeouts.c`.
- Zero-quality false-touch handling: if both dual-capture frames (or an
  error-fallback candidate) score `quality == 0`, the driver drops the image,
  generates no libfprint retry or verify attempt, runs `REARM`, and resumes
  polling; capture errors without a usable frame still surface through
  `fpi_image_device_session_error()`.
- Simplified `REARM` / `RECOVER` state: removed `FpImage *img` retention from
  `RearmData` and `RecoverData`. `RECOVER` preserves `self->capture_submitted`
  across escalation: submitted frames emit finger-off, unsubmitted frames with
  no `capture_error` resume polling directly, and `capture_error` propagates to
  `fpi_image_device_session_error()`.
- Added `libfprint_static` and `libfprint_static_dep` in
  `libfprint/meson.build`, and linked `fpi-sigfm` via `libfprint_static_dep`
  (without `test_utils`) in `tests/meson.build` so mixed public/private API
  tests avoid duplicate enum implementations under `G_MESSAGES_DEBUG=all`.
- Replaced `src/` with the clean `driver/` source layout and removed per-device
  static calibration tables (`gm168_cal.h`) and standalone `src/meson.build`.
- Replaced `patches/libfprint-add-gm168.patch` with
  `patches/libfprint-v1.94.100-integration.patch` targeting pinned upstream
  `libfprint v1.94.100` (`80a4b5ec612892c5c056c48dddaa561452cf37ec`).
- Switched matching from NBIS (`bz3`) to **SIGFM** (`FPI_PRINT_SIGFM`, OpenCV
  SIFT + reciprocal matching + RANSAC affine verification), with 2x bilinear
  output scaling to `160x128`, 20 enrollment stages, enrollment duplicate
  rejection, and bounded verify auto-retry. The versioned `SGFM v1` template
  format remains compatible and unchanged.
- Updated protocol ACK handling to accept `GOODIX_GM168_STATUS_MORE` (`0x03`)
  for `GFUSB_GM168SEC_APP_10036` firmware, and validated dynamic 5-frame dark
  background capture/fallback without requiring calibration files.
- Enforced OpenSSL 3 (`>= 3.0, < 4.0`) dependency selection when `goodix_gm168`
  is enabled so Fedora 45 resolves `openssl3-devel` rather than OpenSSL 4.
- Removed legacy research/Windows/Frida scripts, curl-pipe installer, global
  `ldconfig` wiring, and duplicate `plugdev` udev rules.
- Updated `tools/apply-overlay.sh`, `tests/test_timeouts.c`, `.gitignore`,
  `NOTICE`, `SECURITY.md`, `CONTRIBUTING.md`, `README.md`, `INSTALL.md`,
  `docs/`, and the experimental Fedora RPM spec.

### Fixed
- Background receive now processes every non-empty transfer, drains all ready
  TLS records without extra main-loop sleeps, handles non-timeout USB errors
  without re-listen loops, and propagates TLS failures. Consumed packets
  are removed before capture completion, and stale immediate ACK echoes are
  ignored consistently.
- TLS handshake errors now fail `INIT` promptly with their original cause.
  USB removal and cancellation bypass fallback/recovery cleanly, suppress new
  transfers, and no longer get replaced by stale capture errors.
- Dual-capture error fallback selects the highest-quality image, releases
  the loser, and discards zero-quality fallbacks without triggering a retry.
  `RECOVER` preserves `capture_submitted` across rearm failure—reporting
  finger-off for an already-submitted frame or resuming polling when no frame
  entered the core—instead of storing an image in `RecoverData` or leaving
  the image-device core stranded in `CAPTURE`.
- Early-match client cancellation no longer invalidates an active `REARM` with
  `stale device operation` or leaves the next fast-path activation without FDT:
  the driver drains that SSM and then performs exactly one deferred `DEINIT`.
- Late SIGFM extraction callbacks no longer report enroll progress or identify
  results after a fatal session error/deactivation, and enrollment cannot
  re-enter `AWAIT_FINGER_ON` unless the device is active and `IDLE`. This closes
  the early-submission `REARM`/`RECOVER` failure race without delaying removal.
- FDT threshold loading/calibration is atomic and rejects zero baselines;
  Wallis output is clamped to 16 bits, SSM generation access uses a real common
  base subobject, and obsolete framing/CLAHE code was removed.

### Validation
- An owner-run clean live test of the revised pipeline enrolled `right-thumb`
  (`20/20`), observed the empty-gallery `identify` shortcut and a zero-quality
  dual-capture drop followed by a later match, recorded five `verify-match`
  events, and completed three bounded `REARM` cancellation drains. No `stale
  device operation`, failed `REARM`/`DEINIT`, or watchdog failure occurred. This
  validates the exercised lifecycle paths on one device only; no population
  FAR/FRR or throughput claim is inferred.
- Offline synthetic coverage in `tests/test-fpi-sigfm.c` includes
  `/sigfm/image_device/identify_empty_gallery`,
  `/sigfm/image_device/identify_match_before_finger_off`, and
  `/sigfm/image_device/enroll_fatal_during_scan`; the standalone lifecycle
  predicate matrix and targeted/full Meson suites also pass.
- Earlier offline targeted/full suites for the reciprocal/quality/frame-layout
  revision passed (`8 OK`, `34` disabled-driver/data skips) alongside Valgrind
  checks; translation consensus remains an opt-in synthetic A/B path and stays
  off.
- Live APP 10036 smoke testing (predating these lifecycle and matcher updates)
  completed cold INIT/TLS/BG and `12` successful enroll stages across `15`
  touches before a `G_IO_ERROR_CANCELLED` stop. No TLS, background,
  REARM/RECOVER, or watchdog failure was observed; median dual-capture time was
  `70 ms`. That older run did not exercise the current lifecycle/matcher
  revision. USB hotplug fault injection remains a separate owner-executed
  validation step.

### Known limitations
- Tested only on limited `27c6:589a` hardware (`GFUSB_GM168SEC_APP_10036`) with
  a small FAR/FRR sample set. In one separate owner-run sequence, displaced or
  angled `right-thumb` placements produced high-quality captures but SIGFM scores
  of only about `0..5/8` and repeated `verify-no-match`. The root cause
  (enrollment coverage, pose, or matcher behavior) is not isolated. Client and
  internal retries expanded that sequence to nine scans; reducing retries is a
  possible UX follow-up but does not improve matching. Same-gallery A/B work is
  still required, and no new timing or FAR/FRR claims are made.
- PSK recovery is validated only for the existing-key APP 10036 layout; it does
  not provision a new key and does not provide automatic persistent storage.
- Suspend/resume, USB hotplug, and alternative firmware variants remain
  unvalidated for general use.
