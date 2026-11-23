# Reverse Engineering Methodology and Evidence Boundaries

This document records how the `27c6:589a` protocol, image decoder, and read-only PSK recovery were derived and validated, what is rigorously proven versus unproven, and why certain historical paths were closed.

> **Clean-Room & Exclusion Policy**
>
> This repository contains **only** clean-room C/C++ implementations, synthetic test vectors, and architectural notes. No proprietary Windows driver binaries, firmware images, VM images, raw packet captures (`.pcapng`), disassembly databases, memory address dumps, root secrets, PSKs, DPAPI blobs, or biometric samples are included or documented with raw offsets.

## Project Evolution

The driver architecture evolved through four distinct engineering phases:

1. **Initial Protocol & Image Pipeline (`mrcook1e-ai`)**:
   - Dynamic instrumentation and USB captures of the lawfully installed Windows biometric stack disproved an initial hypothesis that the sensor's `10564`-byte TLS frames were scrambled by a 32-bit LFSR residing in an SGX enclave.
   - Behavioral tracing showed the LFSR path was inactive (`flag == 0`, `seed == 0`, zero runtime calls). Instead, the wire payload is plain 12-bit packed pixel data across `80` padded `132`-byte wire rows, transposed from `80x64` into row-major `64x80` `uint16` frames (`gm168_decode_frame()` in `driver/goodix_gm168.c`).
   - Built the initial `A0`/`B0`/`B2` framing, OpenSSL TLS 1.2 `PSK-AES128-CBC-SHA256` server adapter, and envelope-stretch preprocessor.

2. **Asynchronous Lifecycle Hardening (`yakoshiq` / `libfprint-goodix-gm168`)**:
   - Refactored transport into guarded `FpiSsm` state machines (`INIT`, `CAP`, `REARM`, `RECOVER`, `DEINIT`), centralized timing contracts (`driver/gm168_timeouts.h`), unified USB error classification (`driver/gm168_usb_errors.h`), `io_cancellable`, TLS socketpair cancellation, `2x` bilinear scaling (`160x128`), and 5-frame dark background averaging without mandatory calibration files.

3. **`GFUSB_GM168SEC_APP_10036` Bring-Up & Migration to SIGFM**:
   - Hardware validation on `27c6:589a` with OEM firmware `GFUSB_GM168SEC_APP_10036` identified `GOODIX_GM168_STATUS_MORE` (`0x03`) as a valid `GFUSB` ACK status flag.
   - Closed alternative experimental firmware branches (e.g., `GFUSB_GM168SEC_APP_10019` failed image capture compatibility and was dropped; `GF3206_GM168SEC_APP_10008` strings belong to historical driver notes). `GFUSB_GM168SEC_APP_10036` is the sole validated firmware revision.
   - Replaced NBIS (`MINDTCT`/`Bozorth3`)—which only extracted a median of ~2 minutiae on `80x64` capacitive frames—with `FPI_PRINT_SIGFM` (OpenCV SIFT + Lowe ratio + RANSAC partial-affine verification, `20` enroll stages with anti-duplication, and bounded verify auto-retry), plus portable OpenSSL 3 (`>= 3.0, < 4.0`) build selection.

4. **Read-Only Existing-Key PSK Recovery (Replacing Manual Windows Unsealing & APP-Write Experiments)**:
   - Replaced the legacy workflow that saved `sealed.bin` to disk for manual DPAPI decryption under Windows `NT AUTHORITY\SYSTEM`.
   - Investigated and **closed** APP-mode `0xE0` PSK rotation after static disassembly and controlled testing confirmed the sensor rejects APP-mode `0xE0` writes (`status = 0x01`) and the OEM stack writes PSKs only inside IAP mode after erasing the application image.
   - Replaced both with strictly read-only, in-memory recovery of the already-provisioned key via `0xE4` (`0xbb020001` hash + `0xbb010002` area prefix + adjacent `0xbb010003` white-box record), implemented in pure C (`driver/gm168_psk_recovery.{c,h}`).

## Analysis Methodology

Three complementary clean-room verification techniques were combined without distributing proprietary artifacts:

1. **Static Control-Flow & Format Analysis (Binary Ninja)**:
   - Inspected the Windows user-mode driver and its companion enclave binary statically to map protocol state transitions (`ProcessPsk` validation, IAP vs APP branching, `0xE4` tag semantics), frame restructuring loops, preprocessing math, and the white-box encryption routine (`SecWhiteEncrypt`).
2. **Dynamic Behavioral Tracing (Frida + USB Captures)**:
   - Observed live USB bulk traffic and intermediate Windows preprocessor buffers during lawfully authorized local sessions to verify command ordering (`0x11`, `0x60`, `0x20`, `0xE4`, `0xD0`, `0x90`, `0xC4`, `0xD6`, `0xAE`, `0x32`, `0x34`), `10564`-byte TLS plaintext dimensions, and byte-accurate image preprocessing correspondence.
3. **Synthetic Differential Emulation (Unicorn x64 → Python → Pure C)**:
   - Emulated the white-box encryption routine and its companion enclave counterpart inside an isolated Unicorn x64 harness fed **exclusively with synthetic buffers** (no `DllMain`, no USB imports, no real keys or DPAPI blobs).
   - Derived an independent clean-room Python reference implementation and verified **bit-exact parity** across `13` synthetic Known Answer Tests (KATs) plus a full `0..64` byte length sweep (`65/65` zero-mismatch parity across both Windows components and Python, including the public zero-PSK sentinel vector).
   - Ported the verified algorithm to pure C/OpenSSL 3 in `driver/gm168_psk_recovery.c` and pinned the synthetic KAT vectors in `tests/test-goodix-gm168-psk-recovery.c`.

## Evidence Hierarchy: Proven vs. Not Proven

| Subject | Status | Evidence Basis |
|---|---|---|
| `A0`/`B0`/`B2` USB packet framing, checksums, and `0x00`/`0x01`/`0x03` ACKs | **Proven** | Static analysis, USB traces, and live hardware operation on `GFUSB_GM168SEC_APP_10036` |
| TLS 1.2 `PSK-AES128-CBC-SHA256` handshake + legacy OpenSSL 3 flags (`SECLEVEL=0`, no EMS, no EtM, no ticket/reneg) | **Proven** | Live hardware TLS handshakes and Fedora 45 `OPENSSL_CONF` resolution |
| `10564`-byte frame layout (`80x132` + 4B trailer), 12-bit unpack, `80x64` → `64x80` transpose | **Proven** | Behavioral capture parity (`5120/5120` pixels) and live `SIGFM` capture |
| Dynamic 5-frame dark BG subtraction, `2x` upscale (`160x128`), `SIGFM` extraction/matching | **Proven on 1 device** | Live `20/20` enroll + `5/5` verify + offline frame parity |
| `0xE4` `0xbb020001` hash & `0xbb010002` DPAPI v1 + `0xbb010003` white-box layout | **Proven** | Static analysis + live read-only probe on `GFUSB_GM168SEC_APP_10036` |
| White-box root derivation, `123GOODIX` IV KDF, AES-128-CBC + PKCS#7 + HMAC-SHA256 | **Proven (bit-exact)** | Synthetic differential parity (`Unicorn` host + enclave == Python == C OpenSSL 3) |
| Windows startup flow (validate `0xbb010002` + `0xbb020001`; erase APP via `0xA4` if invalid; re-provision PSK in IAP via `0xE0`; re-flash APP) | **Proven** | Static control-flow analysis of the Windows driver |
| Internal MCU firmware handler logic for `0xE0` / `0xE4` | **Not Proven** | Application firmware binary is MSP430 code without source/symbols; `0xE0` APP write returned generic `0x01` failure |
| MCU acceptance of bare `0xbb010003` without `0xbb010002` DPAPI container in IAP | **Not Proven** | Windows writes both TLVs together in IAP; single-TLV behavior was not tested on hardware |
| Alternative firmware revisions (other than `GFUSB_GM168SEC_APP_10036`) | **Not Proven** | Only `GFUSB_GM168SEC_APP_10036` is validated; `10019` failed capture and is unsupported |
| Cross-device `calib_windows.dat` portability & population-scale FAR/FRR | **Not Proven** | Validated on a single physical sensor and small sample set only |
| Suspend/resume, hotplug, and long-term cold-boot stability | **Not Proven** | Not broadly validated across power cycles or diverse hosts |

## OEM `GFUSB_GM168SEC_APP_10036` & Windows Re-Provisioning Behavior

Understanding how the Windows biometric stack manages the sensor's PSK is critical for dual-boot users and Linux reliability:

1. **Normal Windows boot (valid PSK)**:
   - On initialization, Windows reads `0xbb010002` (host-sealed DPAPI/SGX copy) and `0xbb020001` (MCU PSK SHA-256) via command `0xE4`, unseals the host copy via DPAPI (`LOCAL SERVICE` context), and verifies `SHA256(unsealed) == mcu_hash`.
   - When validation passes, Windows performs **no** flash erase, firmware write, or PSK rotation.

2. **Windows recovery / re-provisioning (missing or invalid DPAPI copy)**:
   - The device PSK is **not** an immutable factory burn-in secret. If DPAPI unsealing fails (e.g., fresh Windows installation, changed Windows installation identity, or corrupted/missing `0xbb010002` container), Windows treats the host copy as lost.
   - In that state, the Windows driver issues `McuEraseApp` (`0xA4`) to transition the MCU into IAP mode, generates a fresh random 32-byte PSK, wraps it in both a new DPAPI `0xbb010002` blob and a new white-box `0xbb010003` TLV, writes the container via `0xE0` **inside IAP mode**, and re-flashes the application firmware with an HMAC computed from the new PSK.
   - **Impact on Linux**: any prior `/etc/goodix-gm168/psk.bin` backup becomes stale after Windows re-provisioning. Because the Linux driver fails closed on hash mismatch (see [`PSK-RECOVERY.md`](PSK-RECOVERY.md)), a stale `psk.bin` must be moved aside deliberately so read-only recovery can extract the newly provisioned PSK into memory.

3. **Why Linux avoids write/IAP provisioning**:
   - Hardware testing confirmed `GFUSB_GM168SEC_APP_10036` rejects `0xE0` PSK writes in APP mode (`status = 0x01`), matching static evidence that the OEM driver only writes PSKs inside IAP after erasing APP.
   - Because read-only `0xE4` recovery cleanly extracts the existing PSK in memory without touching MCU NVRAM or flash, `goodix_gm168` intentionally omits all `0xA4`/`0xE0`/`0xF0`/`0xF4` erase/write/flash paths. Do **not** attempt experimental firmware flashing or IAP scripting on working hardware.
