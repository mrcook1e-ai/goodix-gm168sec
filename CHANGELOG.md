# Changelog

All notable changes to the `goodix-gm168` overlay are documented in this file.

## [Unreleased]

### Added
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
- Dual-capture error fallback selects the highest-quality image and releases
  the loser. `RECOVER` preserves a completed image across rearm failure and
  terminates empty retry captures through `retry_scan()` instead of leaving
  the image-device core in `CAPTURE`.
- FDT threshold loading/calibration is atomic and rejects zero baselines;
  Wallis output is clamped to 16 bits, SSM generation access uses a real common
  base subobject, and obsolete framing/CLAHE code was removed.

### Validation
- Offline targeted/full suites for the revised matcher passed (`8 OK`, `34`
  disabled-driver/data skips), including the new frame-layout target; a clean
  pinned-checkout overlay rebuild passed all four targeted suites, the standalone
  timeout contract passed, and current direct/Meson Valgrind runs completed with
  zero errors and zero definitely/indirectly/possibly lost blocks.
- The reciprocal/quality/deferred-deactivation revision has **not** received a
  post-change live hardware run. Its current evidence is synthetic only;
  translation consensus has only an opt-in synthetic A/B path and stays off.
- Live APP 10036 smoke testing completed cold INIT/TLS/BG and `12` successful
  enroll stages across `15` touches before a `G_IO_ERROR_CANCELLED` stop. No
  TLS, background, REARM/RECOVER, or watchdog failure was observed; median
  dual-capture time was `70 ms`. A full `20/20` enroll and USB hotplug fault
  injection remain separate validation steps.

### Known limitations
- Tested only on limited `27c6:589a` hardware (`GFUSB_GM168SEC_APP_10036`) with
  a small FAR/FRR sample set. Revised reciprocal/quality/lifecycle behavior and
  the provisional `30`/`0.55` capture gate still need same-frame live A/B
  validation; no new accuracy claim is made.
- PSK recovery is validated only for the existing-key APP 10036 layout; it does
  not provision a new key and does not provide automatic persistent storage.
- Suspend/resume, USB hotplug, and alternative firmware variants remain
  unvalidated for general use.
