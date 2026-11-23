# Agent and Maintainer Guide (`goodix-gm168sec`)

This repository is a public source overlay for the experimental **Goodix GM168SEC** (`27c6:589a`) `libfprint` driver, `FPI_PRINT_SIGFM` core integration, and read-only OpenSSL 3 PSK recovery. Any coding agent or contributor modifying this repository must strictly follow the layout, build/test, memory-hygiene, review, and safety policies below.

## Canonical Repository Layout

Keep changes strictly separated by responsibility:

- `driver/` — Canonical C11 driver sources and headers (`goodix_gm168.c`, `goodix_proto.{c,h}`, `goodix_tls.{c,h}`, `gm168_psk_recovery.{c,h}`, `gm168_frame_layout.h`, `gm168_timeouts.h`, `gm168_usb_errors.h`, `gm168_trace.h`, `gm168_log.{c,h}`). Copied directly into `libfprint/drivers/goodix_gm168/` by `tools/apply-overlay.sh`.
- `patches/libfprint-v1.94.100-integration.patch` — Integration patch against pinned upstream `libfprint v1.94.100` (`80a4b5ec612892c5c056c48dddaa561452cf37ec`). Houses `libfprint/sigfm/*`, core `FPI_PRINT_SIGFM` hooks, `tests/test-fpi-sigfm.c`, `tests/test-goodix-gm168-frame-layout.c`, OpenSSL 3 build policy (`>= 3.0, < 4.0`), `libfprint/meson.build` / `tests/meson.build` driver wiring, and `fprint-list-udev-hwdb.c` allowlist cleanup.
- `tools/apply-overlay.sh` — Enforces clean `v1.94.100` target checkout, copies `driver/*` and `tests/test-goodix-gm168-psk-recovery.c`, checks and applies `patches/libfprint-v1.94.100-integration.patch`.
- `tests/` — Standalone compile-time timeout contract check (`tests/test_timeouts.c`). PSK recovery tests (`tests/test-goodix-gm168-psk-recovery.c`), SIGFM tests (`tests/test-fpi-sigfm.c`, `libfprint/sigfm/tests.cpp`), and the packed-frame layout test (`tests/test-goodix-gm168-frame-layout.c`) run inside the patched `libfprint` build tree.
- `packaging/` — Udev `uaccess` rule (`packaging/udev/70-goodix-gm168.rules`) and experimental Fedora RPM spec (`packaging/fedora/libfprint-goodix-gm168.spec`).
- `docs/` & root policies (`README.md`, `INSTALL.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, `NOTICE`) — Curated technical documentation and safety boundaries.

Never recreate legacy `src/`, `gm168_cal.h`, Windows Frida/unseal scripts, or experimental `10019` / `0xE0` flash scripts in this repository.

## Mandatory Build, Test, and Patch Commands

### 1. Apply Overlay and Configure Isolated Build

Use a clean pinned `libfprint v1.94.100` checkout and an isolated build/prefix directory outside system paths:

```bash
git clone --branch v1.94.100 --depth 1 \
  https://gitlab.freedesktop.org/libfprint/libfprint.git ./libfprint-src

./tools/apply-overlay.sh ./libfprint-src

meson setup ./build ./libfprint-src \
  --prefix="$PWD/prefix" \
  -Ddrivers=goodix_gm168 \
  -Dgtk-examples=false \
  -Ddoc=false \
  -Dinstalled-tests=false
```

Verify OpenSSL 3 (`>= 3.0, < 4.0`) and OpenCV 4 (`>= 4.5.0`) before compiling:

```bash
pkg-config --modversion openssl
pkg-config --modversion opencv4
```

### 2. Mandatory Targeted + Full Test Verification

Every change touching driver logic, PSK recovery, SIGFM, or build files **must** run both targeted suites and the full test run:

```bash
# Compile
meson compile -C ./build

# Targeted driver, recovery, and SIGFM suites
meson test -C ./build \
  goodix-gm168-psk-recovery fpi-sigfm sigfm-tests goodix-gm168-frame-layout \
  --print-errorlogs

# Full meson suite (expect all enabled targets OK; disabled drivers skipped)
meson test -C ./build --print-errorlogs

# Standalone timeout _Static_assert check
cc -std=c11 -Wall -Wextra -Werror -Idriver tests/test_timeouts.c -o /tmp/test_timeouts
/tmp/test_timeouts
rm -f /tmp/test_timeouts

# Patch / whitespace sanity in the overlay repo
git diff --check
```

### 3. Memory and Zeroization Checks

Changes touching cryptography, DPAPI/TLV parsing, USB reply validators, `SGFM` serialization, or heap buffers **must** additionally verify memory safety and zeroization:

```bash
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
  ./build/tests/test-goodix-gm168-psk-recovery

meson test -C ./build --setup=valgrind \
  goodix-gm168-psk-recovery fpi-sigfm sigfm-tests goodix-gm168-frame-layout \
  --print-errorlogs
```

Confirm:
- Secret buffers (`psk_area`, `psk_expected_hash`, transfer buffers, intermediate keys/plaintexts, `goodix_gm168_psk`) are scrubbed via `OPENSSL_cleanse()`.
- Secret/hash/HMAC/IV/sentinel comparisons use constant-time `CRYPTO_memcmp()`.
- OpenSSL error queues are drained (`ERR_clear_error()`, `ERR_peek_error() == 0`).
- `GError` messages and logs contain **zero** key bytes, DPAPI bytes, white-box bytes, or OpenSSL error details.

### 4. Regenerating the Integration Patch (Excluding Overlay-Owned Sources)

Never mix `driver/` changes or `tests/test-goodix-gm168-psk-recovery.c` into `patches/libfprint-v1.94.100-integration.patch`. Whenever core `libfprint`, `libfprint/sigfm/`, `meson.build`, or upstream-owned tests change inside a patched `libfprint` checkout are finalized, regenerate the patch while excluding both overlay-owned source paths:

```bash
# Include new untracked SIGFM/test files without committing or content-staging.
git -C ./libfprint-src add -N \
  libfprint/sigfm tests/test-fpi-sigfm.c tests/test-goodix-gm168-frame-layout.c

git -C ./libfprint-src diff \
  80a4b5ec612892c5c056c48dddaa561452cf37ec \
  -- ':!libfprint/drivers/goodix_gm168/**' \
     ':!tests/test-goodix-gm168-psk-recovery.c' \
  > patches/libfprint-v1.94.100-integration.patch

git diff --check
```

## Code Style and Architectural Rules

- **GNU C99 (`driver/`, `libfprint` C core)**: follow `libfprint` / GLib idioms (`g_autofree`, `g_autoptr`, `GError` propagation, no large 16 KiB stack arrays). Preserve `LGPL-2.1-or-later` SPDX headers and attribution in `NOTICE`.
- **OpenSSL 3 compatibility**: use `EVP_Digest()`, `EVP_Cipher*` / `EVP_Encrypt*` / `EVP_Decrypt*`, `HMAC()`, `OPENSSL_cleanse()`, and `CRYPTO_memcmp()`; never use deprecated OpenSSL 1.1 APIs such as `HMAC_CTX`. Keep the Meson OpenSSL `>= 3.0, < 4.0` check intact.
- **C++17 (`libfprint/sigfm/`)**: validate bounds, magic/version, and IEEE-754 finiteness (`std::isfinite`) on all deserialized keypoints/descriptors; keep `sigfm-tests` deterministic on synthetic patterns only. Production matching must retain reciprocal filtering; `SIGFM_MATCH_FLAG_TRANSLATION_CONSENSUS` and `SIGFM_MATCH_FLAG_FORWARD_ONLY` are offline A/B controls, not driver defaults.
- **Async USB & SSM invariants**: every USB transfer and timer must route through `FpiSsm`, check `gm168_ssm_is_current()` (generation guard) and `gm168_ssm_deadline_expired()`, use `self->io_cancellable`, and classify errors via `gm168_classify_usb_error()`. Update matching `_Static_assert`s in `driver/gm168_timeouts.h` and `tests/test_timeouts.c` if any timing constant changes.
- **Read-only recovery invariants**: `driver/gm168_psk_recovery.c` must remain pure in-memory C (no USB, no filesystem I/O). Never hard-code DPAPI offsets (always parse dynamically), never weaken zero-PSK sentinels or canonical IV/HMAC checks, and never trigger recovery after a TLS failure.

## Review Policy

- **Default independent review**: use **Gemini 4 Neon** for independent code, safety, and documentation reviews before finalizing changes.
- **Escalation (`Argon`)**: reserve **Argon** for exceptionally heavy follow-up analysis only when Neon review leaves a specific unresolved high-risk question and the user authorizes that escalation.

## Strict Safety Prohibitions (Require Explicit User Permission)

Do **not** perform any of the following without explicit, separate user authorization:

1. **Live hardware or daemon mutations**: never access USB `27c6:589a`, run `fprintd-enroll` / `fprintd-verify` / `fprintd-delete`, run `meson install` into system/opt prefixes, modify `/etc/` or systemd drop-ins, or restart `fprintd.service`.
2. **Git history / remote publishing**: never run `git commit` or `git push`.
3. **Destructive MCU / provisioning operations**: never implement or send `0xA4` (erase), `0xE0` (PSK write), `0xF0`/`0xF4` (firmware flash), or IAP scripts, and never persist `psk.bin` or `sealed.bin` automatically from driver code.

## Secrets, Biometric, and Proprietary OEM Exclusions

Before proposing, staging, or documenting any file, verify it contains **none** of the following:

- Real device PSKs, PSK SHA-256 hashes, white-box root secrets, `sealed.bin`, or Windows DPAPI blobs;
- Per-device calibration or state files (`calib_windows.dat`, `cal_scale.bin`, `fdt_thresholds.bin`) or per-device constants;
- Real fingerprint images (`.pgm`, `.png`, `.bmp`, `.raw`, `GM168_DUMP_FRAMES` output) or serialized `/var/lib/fprint/` templates;
- Raw USB/TLS packet captures (`.pcapng`), `GM168_LOG_DIR` logs, or personal local filesystem paths/IPs/usernames;
- Proprietary Windows driver binaries, companion enclaves, `.inf`/`.cat`/`.sys` packages, firmware blobs, VM images, or raw binary address/RVA dumps.

All unit tests and KATs must use strictly synthetic inputs and fixtures.

## Documentation Synchronization Rules

- Whenever state machines, protocol constants, PSK recovery gates, SIGFM parameters, or build/test procedures change, update the matching documents under `docs/` (`README.md`, `ARCHITECTURE.md`, `STATE-MACHINES.md`, `PROTOCOL.md`, `PSK-RECOVERY.md`, `IMAGE-PIPELINE.md`, `SIGFM.md`, `TESTING.md`, `DEVELOPMENT.md`, `SECURITY.md`, `FAQ.md`), root `README.md`, `INSTALL.md`, `CHANGELOG.md`, and `AGENTS.md`.
- Always distinguish **offline synthetic proof** from **live hardware proof**.
- Never fabricate hardware support, firmware compatibility, suspend/resume reliability, or FAR/FRR claims; document all unvalidated boundaries explicitly.
