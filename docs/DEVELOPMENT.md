# Development Guide

## Repository structure

- `driver/` — canonical `goodix_gm168` driver C files copied into
  `libfprint/libfprint/drivers/goodix_gm168/`
- `patches/libfprint-v1.94.100-integration.patch` — diff against upstream
  `libfprint v1.94.100` (`80a4b5ec612892c5c056c48dddaa561452cf37ec`) adding
  SIGFM (`libfprint/sigfm/`), `FPI_PRINT_SIGFM` core support, `test-fpi-sigfm`,
  OpenSSL 3 (`>= 3.0, < 4.0`) dependency logic, and `fprint-list-udev-hwdb`
  allowlist cleanup (`27c6:589a`)
- `tools/apply-overlay.sh` — copies `driver/*` and the synthetic recovery test,
  then applies the patch to a clean pinned `libfprint v1.94.100` checkout
- `tests/test_timeouts.c` — standalone compile-time check of `gm168_timeouts.h`
- `tests/test-goodix-gm168-psk-recovery.c` — synthetic DPAPI/TLV/OpenSSL and E4
  reply tests; Meson links the pure recovery module without accessing USB
- `packaging/` — `uaccess` udev rule and experimental Fedora RPM spec

## Applying and testing changes

1. Obtain a clean `libfprint v1.94.100` checkout (`80a4b5ec612892c5c056c48dddaa561452cf37ec`).
2. Run `./tools/apply-overlay.sh /path/to/libfprint`.
3. Configure, compile, and run tests in an isolated build directory:

```bash
meson setup build /path/to/libfprint \
  --prefix="$PWD/prefix" \
  -Ddrivers=goodix_gm168 \
  -Dgtk-examples=false \
  -Ddoc=false \
  -Dinstalled-tests=false
meson compile -C build
meson test -C build --print-errorlogs
```

4. Check the timeout constants contract:

```bash
cc -std=c11 -Wall -Wextra -Werror -Idriver tests/test_timeouts.c -o /tmp/test_timeouts
/tmp/test_timeouts
rm -f /tmp/test_timeouts
```

To verify `libfprint` links only OpenSSL 3 (`libssl.so.3` / `libcrypto.so.3`) on
Fedora 45, inspect `ldd build/libfprint/libfprint-2.so.2*`.

## Updating the integration patch

Keep driver-only changes in `driver/` and avoid mixing them into
`patches/libfprint-v1.94.100-integration.patch`. When core/SIGFM/build files in a
patched `libfprint` tree change, regenerate the patch while excluding both
overlay-owned sources: `libfprint/drivers/goodix_gm168/**` and
`tests/test-goodix-gm168-psk-recovery.c`:

```bash
git -C /path/to/libfprint diff \
  80a4b5ec612892c5c056c48dddaa561452cf37ec \
  -- ':!libfprint/drivers/goodix_gm168/**' \
     ':!tests/test-goodix-gm168-psk-recovery.c' \
  > patches/libfprint-v1.94.100-integration.patch
git diff --check
```

## Diagnostic and tuning environment variables

All diagnostic hooks are off by default:

- `GOODIX_GM168_DIR` — override `/etc/goodix-gm168` for isolated non-root runs
- `GM168_LOG_DIR` — enable private per-session protocol text logs (`0700` dir,
  `0600` files)
- `GM168_TRACE=1` — emit `[TRACE]` state-transition timings via `g_warning`
- `GM168_DUMP_FRAMES=<dir>` — dump intermediate `.pgm` and `-meta.txt` stage
  diagnostics (treat output as sensitive biometric data; never publish)
- `GM168_DUP_THRESH` — override enrollment anti-duplicate SIGFM score threshold
  (`0..1000`, default `15`)
- `GM168_VERIFY_RETRIES` — override verify/identify auto-retry count (`0..5`,
  default `2`)
- `GM168_BG_MIN_MEAN`, `GM168_BG_RETRIES` — dark background validation mean floor
  and retry cap
- `GM168_CAPTURE_SETTLE_MS`, `GM168_WAIT_LIFT_POLL_MS` — capture settle and
  lift-poll interval overrides

## Remaining blockers before general release / upstreaming

- Independent hardware validation on additional `27c6:589a` devices and other
  firmware versions besides `GFUSB_GM168SEC_APP_10036`.
- Independent hardware validation of read-only PSK recovery; automatic key
  persistence and new-key provisioning remain deliberately separate concerns.
- Population-scale FAR/FRR evaluation of SIGFM thresholds and enrollment stages.
- Suspend/resume, USB hotplug, and cancellation stress testing on physical units.
- Upstream `libfprint` umockdev driver test fixtures and `autosuspend.hwdb` sync.
