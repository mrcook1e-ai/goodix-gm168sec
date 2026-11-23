# Testing and Validation Guide

This guide covers building the `goodix_gm168` overlay in an isolated directory, running the offline synthetic test suites and memory checks, understanding the boundary between offline and live hardware proof, and performing safe user-driven validation on physical hardware.

## Offline Proof vs. Live Proof

| Verification Level | What It Proves | What It Does **Not** Prove | Hardware / Privileges Needed |
|---|---|---|---|
| **Offline Synthetic Suites** (`meson test`, `test_timeouts.c`, Valgrind) | Timeout `_Static_assert` contracts; OpenSSL 3 dependency enforcement; SIGFM SIFT/RANSAC/serialization math; `FPI_PRINT_SIGFM` `FP3` integration; DPAPI v1/TLV parsing, white-box crypto, zero-sentinel gates, `0xE4` reply validators, and secret buffer cleansing on synthetic vectors | Physical USB packet timing on a specific laptop host; actual MCU responses for untested firmware revisions; cold-reboot/suspend persistence; real-finger FAR/FRR across diverse populations | None (unprivileged, zero USB access) |
| **Live Hardware Validation** (human user with `27c6:589a`) | End-to-end USB `A0`/`B0`/`B2` transport, `0xE4` recovery/hash verification, TLS 1.2 handshake on host OpenSSL 3, dark BG capture, `20`-stage enrollment, `verify` reliability, cold reboot, suspend/resume, and local impostor rejection | Cross-device portability across arbitrary hardware lots, unconfirmed firmware variants, or population-scale FAR/FRR | Physical `27c6:589a` sensor (`GFUSB_GM168SEC_APP_10036`) + interactive user |

## 1. Isolated Overlay Apply and Build

Never install unreviewed test builds into `/usr/lib*` or alter global `ldconfig` paths. Build against a clean checkout of pinned `libfprint v1.94.100`:

```bash
git clone --branch v1.94.100 --depth 1 \
  https://gitlab.freedesktop.org/libfprint/libfprint.git ./libfprint-src

./tools/apply-overlay.sh ./libfprint-src

pkg-config --modversion openssl   # must report >= 3.0 and < 4.0
pkg-config --modversion opencv4   # must report >= 4.5.0

meson setup ./build-gm168 ./libfprint-src \
  --prefix="$PWD/prefix" \
  -Ddrivers=goodix_gm168 \
  -Dgtk-examples=false \
  -Ddoc=false \
  -Dinstalled-tests=false

meson compile -C ./build-gm168
```

Verify that the compiled `libfprint` shared library links against OpenSSL 3 (`libssl.so.3` / `libcrypto.so.3`) rather than OpenSSL 4:

```bash
ldd ./build-gm168/libfprint/libfprint-2.so.2* | grep -E 'ssl|crypto|opencv'
```

## 2. Offline Synthetic KAT, Unit, and Negative Test Suites

### Run Full Meson Test Suite

```bash
meson test -C ./build-gm168 --print-errorlogs
```

When `doctest` (`>= 2.0.0, < 3.0.0`) and `cairo` are present, this executes the full synthetic suite (tests for disabled drivers report `SKIP`, while enabled targets must pass):

| Test Target | Source | Coverage Summary |
|---|---|---|
| `goodix-gm168-psk-recovery` | `tests/test-goodix-gm168-psk-recovery.c` (links `gm168_psk_recovery.c` without USB) | Derived 16-byte root secret SHA-256 fingerprint; public `gm168_public_zero_psk_whitebox` sentinel round-trip; `14` pinned synthetic KAT decrypts + `0..100` B length sweep; rejection of bad blob lengths, tampered HMACs, corrupt PKCS#7 padding, non-canonical IVs; clean `ERR_peek_error() == 0`; DPAPI v1 dynamic field-order parsing, bit-length algorithm fields, early cumulative-bounds rejection (`3992` B max), and progressive `NEED_MORE` prefixes; exact `104`-byte `0xbb010003` TLV checks; `0xbb020001` hash mismatch zeroization of `psk_out`; zero-PSK hash/record sentinels; `0xE4` hash & area reply validators |
| `sigfm-tests` | `libfprint/sigfm/tests.cpp` (`doctest`) | Null/empty/blank image handling; `SGFM` v1 serialization round-trip & copy isolation; rejection of truncated, trailing, bad magic/version, oversized count (`> 16384`), and `NaN`/`Inf` floats; synthetic affine-transformed genuine vs impostor pattern scoring; RANSAC `scale` (`[0.80, 1.25]`) and `inlier_ratio` (`>= 0.40`) boundaries |
| `fpi-sigfm` | `tests/test-fpi-sigfm.c` | Async `GTask` extraction + `H_FLIPPED`/`V_FLIPPED`/`COLORS_INVERTED` normalization (`PARTIAL` preserved); concurrent `G_IO_ERROR_ADDRESS_IN_USE` guard; `FP3` round-trip & corrupt variant rejection; strict `FPI_PRINT_SIGFM` vs `FPI_PRINT_NBIS` compatibility rejection; enroll anti-dup `max_score`; verify auto-retry counter & reset semantics |
| `fpi-device`, `fpi-ssm`, `fpi-assembling` | Upstream `libfprint` core tests | `FpDevice` / `FpiSsm` / image assembling core invariants |

### Run Targeted Subsets Quickly

```bash
meson test -C ./build-gm168 \
  goodix-gm168-psk-recovery fpi-sigfm sigfm-tests \
  --print-errorlogs
```

### Standalone Timeout Contract Check

From the repository root, compile and execute `tests/test_timeouts.c` to validate `_Static_assert` invariants in `driver/gm168_timeouts.h`:

```bash
cc -std=c11 -Wall -Wextra -Werror -Idriver tests/test_timeouts.c -o /tmp/test_timeouts
/tmp/test_timeouts
rm -f /tmp/test_timeouts
```

## 3. Memory, Leak, and Zeroization Verification

Because PSK recovery and SIGFM manipulate heap buffers and secret keys, run memory checks after modifying C/C++ buffers, parsers, or OpenSSL calls:

### Valgrind Leak & Undefined-Read Checks

```bash
meson test -C ./build-gm168 \
  --setup=valgrind \
  goodix-gm168-psk-recovery fpi-sigfm sigfm-tests \
  --print-errorlogs
```

Or run a single binary directly with strict leak reporting:

```bash
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
  ./build-gm168/tests/test-goodix-gm168-psk-recovery
```

### AddressSanitizer + UndefinedBehaviorSanitizer (Optional Isolated Build)

```bash
meson setup ./build-asan ./libfprint-src \
  -Ddrivers=goodix_gm168 \
  -Dgtk-examples=false -Ddoc=false -Dinstalled-tests=false \
  -Db_sanitize=address,undefined
meson test -C ./build-asan \
  goodix-gm168-psk-recovery fpi-sigfm sigfm-tests \
  --print-errorlogs
```

### Code-Level Zeroization & Side-Channel Audit Checklist

- All temporary key, plaintext, DPAPI area, and USB transfer buffers in recovery paths are wiped with `OPENSSL_cleanse()` before freeing or returning.
- Hash, HMAC, IV, and white-box sentinel comparisons use constant-time `CRYPTO_memcmp()`.
- On any failure path in `gm168_psk_recover_from_area()`, `psk_out` is explicitly cleansed and `*record_offset_out` is reset to `0`.
- `GError` messages report only lengths, tags, and error codes—never raw PSKs, DPAPI bytes, white-box bytes, or OpenSSL error strings.

## 4. Experimental Fedora RPM `%check`

[`packaging/fedora/libfprint-goodix-gm168.spec`](../packaging/fedora/libfprint-goodix-gm168.spec) includes a `%check` section that executes `%meson_test --print-errorlogs` and compiles/runs `tests/test_timeouts.c` with `%{optflags} -std=c11 -Wall -Wextra -Werror`. Because this package sets `Conflicts: libfprint` and replaces the system library, test RPM builds only inside disposable mock/container environments (e.g., `mock` or `rpmbuild` in a container) rather than overwriting a daily-driver host blindly.

## 5. Safe User-Executed Hardware Validation

Only the device owner should execute live hardware tests on a physical `27c6:589a` reader. Never run raw USB diagnostic scripts concurrently while `fprintd` holds the device.

### Pre-Flight Checklist

1. Connect AC power and close any virtual machines or USB-passthrough sessions.
2. Confirm the sensor is running `GFUSB_GM168SEC_APP_10036` (never flash experimental firmware).
3. Ensure the udev `uaccess` rule (`packaging/udev/70-goodix-gm168.rules`) is installed if running standalone `libfprint` tools as a non-root user, or use an isolated `$GOODIX_GM168_DIR` (`0700`) for non-root testing.
4. On **Fedora 45**, ensure the `fprintd` systemd drop-in sets `Environment=OPENSSL_CONF=/etc/pki/tls/openssl3.cnf` (scoped strictly to `fprintd.service`; do not weaken system-wide `crypto-policies`).
5. **Do not** enable `GM168_DUMP_FRAMES` or `GM168_LOG_DIR` unless actively debugging locally; never upload raw outputs.

### Recommended Manual Validation Sequence

1. **Clean migration from NBIS**: if old NBIS prints exist, delete them first (`fprintd-delete <user>`).
2. **Activation & enrollment (`20` stages)**: run `fprintd-enroll -f right-index-finger <user>`. Touch calmly for ~0.5 s, lift completely between stages, and shift finger placement across tip, center, left/right edges, and base to cover the `4 × 3.2 mm` sensor and satisfy the anti-duplicate check (`GM168_SIGFM_DUP_THRESHOLD = 15`).
3. **Verification series**: run `fprintd-verify -f right-index-finger <user>` across multiple centered and slightly shifted touches to observe first-touch match and auto-retry (`GM168_SIGFM_VERIFY_RETRIES = 2`).
4. **Impostor (FAR) check**: test `5+` unenrolled fingers (your other fingers and/or consenting testers) and confirm `verify-no-match`.
5. **Lifecycle resilience**: verify after a cold reboot (which clears MCU volatile state and exercises fresh pre-TLS `0xE4` probe/recovery) and across `3+` `systemctl suspend` / resume cycles before relying on fingerprint unlock. Always keep password authentication enabled in PAM.
