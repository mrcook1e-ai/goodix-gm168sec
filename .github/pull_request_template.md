## Description

Briefly describe the change and its rationale.

## Type of Change

- [ ] Driver logic (`driver/`)
- [ ] Core integration patch (`patches/libfprint-v1.94.100-integration.patch`)
- [ ] Synthetic unit tests / timeout check (`tests/`)
- [ ] Packaging / udev rules (`packaging/`)
- [ ] Documentation (`docs/`, `README.md`, `INSTALL.md`)

## Mandatory Verification Checklist

Before submitting, verify that all applicable tests and hygiene checks pass:

- [ ] **Timeout contract**: `gcc -std=c11 -Wall -Wextra -Werror -Idriver tests/test_timeouts.c -o /tmp/test_timeouts && /tmp/test_timeouts` succeeds without warnings.
- [ ] **Whitespace & git hygiene**: `git diff --check` reports zero warnings.
- [ ] **Build & targeted tests**: `meson test -C build goodix-gm168-psk-recovery fpi-sigfm sigfm-tests goodix-gm168-frame-layout --print-errorlogs` passes.
- [ ] **Full test suite**: `meson test -C build --print-errorlogs` passes.
- [ ] **Memory & leak checks**: Valgrind reports zero leaks and clean zeroization on secret buffers.
- [ ] **Overlay separation**: If core files were modified, `patches/libfprint-v1.94.100-integration.patch` was regenerated excluding `driver/**` and `tests/test-goodix-gm168-psk-recovery.c`.

## Strict Security & Secrets Boundary

- [ ] I confirm this PR contains **no** real device PSKs, PSK hashes, DPAPI records, or white-box secrets.
- [ ] I confirm this PR contains **no** per-device calibration files or biometric fingerprint images (`.pgm`, `.png`, `.bmp`, `.raw`).
- [ ] I confirm this PR contains **no** raw USB packet captures (`.pcapng`) or proprietary Windows binaries/driver files.
- [ ] Cryptographic comparisons use constant-time `CRYPTO_memcmp()`.
- [ ] Secret buffers are scrubbed using `OPENSSL_cleanse()`.
