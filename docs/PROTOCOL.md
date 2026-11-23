# GM168 protocol notes

This document describes the protocol and image-processing behavior implemented
in `driver/`. It was derived from USB traffic and behavioral analysis of the
Windows stack and contains no PSK, calibration table, biometric sample, raw
frame, or proprietary binary.

## USB interface

| Item | Value |
|---|---:|
| VID:PID | `27c6:589a` |
| Confirmed firmware | `GFUSB_GM168SEC_APP_10036` |
| Interface | `0` |
| Bulk OUT | `0x01` |
| Bulk IN | `0x83` |
| OUT chunk | 64 bytes |
| Maximum driver receive buffer | 16 KiB |

The implementation recognizes three outer packet types:

- `A0` — command, ACK, and finger-detection traffic;
- `B0` — TLS records transported over USB;
- `B2` — encrypted image records.

`A0` contains a little-endian inner length, header checksum, command/payload
section, and body checksum. Exact encoding and validation live in
`driver/goodix_proto.c`. Command ACKs accept `0x00` (`OK`), `0x01` (`BUSY`), and
`0x03` (`MORE`, emitted by `GFUSB` firmware when an additional packet follows).

## Session outline

The high-level activation sequence is:

1. wake (`0x11`) and session reset (`0x60`);
2. firmware/version query (`0x20`);
3. read the fresh PSK SHA-256 through `0xE4` (`0xbb020001`); if `psk.bin` is
   absent, recover the existing key from a bounded `0xbb010002` area walk,
   validate the dynamic DPAPI v1 layout and exact 104-byte `0xbb010003` record,
   and verify the recovered SHA-256 before using it in memory;
4. TLS-PSK handshake over `B0` packets (`0xD0`);
5. sensor register setup (`0x90`), template slot cleanup (`0xC4`), session init
   (`0x60`), power (`0xD6`), and IRQ/FDT setup (`0xAE`, `0x32`);
6. five no-finger dark background frames (`0x20` trigger loop) with mean
   sanity check (`GM168_BG_MIN_MEAN`, default `2200`);
7. FDT re-arm (`0x34` x2, delay, `0xAE`, `0x32`);
8. finger-detection polling on Bulk IN (`0x83`);
9. settle delay (`150 ms` default), capture trigger (`0x20`), and `B2` receive;
10. dual-capture selection, sensor re-arm, and finger-lift wait (`wait_lift`).

Every transport phase is asynchronous through libfprint `FpiSsm` state
machines. Deadlines, retry caps, cancellation, removal handling, and recovery
paths are defined in `driver/gm168_timeouts.h`, `driver/gm168_usb_errors.h`,
and `driver/goodix_gm168.c`.

## TLS

The sensor behaves as a legacy TLS 1.2 PSK client while the host driver runs
the server side. The implemented cipher suite is:

```text
PSK-AES128-CBC-SHA256
```

OpenSSL 3 (`>= 3.0, < 4.0`) is configured with `SECLEVEL=0` and legacy
compatibility options (`SSL_OP_NO_EXTENDED_MASTER_SECRET`,
`SSL_OP_NO_ENCRYPT_THEN_MAC`, `SSL_OP_NO_RENEGOTIATION`, `SSL_OP_NO_TICKET`) to
match the sensor's TLS 1.0-era handshake framing and MAC-then-Encrypt CBC
behavior.

The device-specific 32-byte PSK is loaded at runtime from `psk.bin` or recovered
read-only into memory when that file is absent; it is never compiled into the
source or persisted by the recovery path. A recovered key is accepted only after
its SHA-256 matches the fresh sensor reply in constant time. Recovery does not
run as a retry after TLS failure. The OpenSSL adapter uses an `AF_UNIX` socket
pair so the blocking `SSL_accept` handshake runs on a helper thread with
`SO_RCVTIMEO` while USB I/O remains asynchronous on the GLib main loop.

## Image wire format and scaling

One decrypted frame is `10564` bytes:

- 80 wire rows;
- 132 bytes per wire row;
- first 96 bytes contain 64 packed 12-bit pixels;
- remaining 36 bytes are row padding;
- final 4 bytes are a trailer.

Unpacking and transposition produce an `80x64` (`5120`-pixel) logical frame.
Because the native sensor area is very small, the driver performs an internal
2x bilinear enlargement (`GM168_IMAGE_SCALE = 2`) to **`160x128`** before
passing the `FpImage` to libfprint.

## Dynamic background fallback and optional calibration

- **Dynamic background fallback (default)**: when no calibration files are
  present, the driver averages 5 dark frames captured at initialization,
  subtracts the per-pixel dark reference (`raw - bg + 0x800`), applies a 3x3
  median filter, runs horizontal/vertical 11-pixel envelope estimation + 5-point
  X morphological close + local contrast normalization, and outputs an inverted
  8-bit grayscale image (`FPI_IMAGE_COLORS_INVERTED`).
- **Optional per-device calibration**: if matching `calib_windows.dat` (140464
  bytes) and `cal_scale.bin` (10240 bytes) are placed in the private state
  directory, the driver validates dimensions, 12-bit ranges, nonzero divisors,
  plane ordering, and empty padding, then applies:

  ```text
  corrected[i] = round((bright_reference[i] - raw[i]) * 8192 / scale[i])
  ```

  followed by 2..98 percentile normalization (`FPI_IMAGE_PARTIAL`).

## Matching (`FPI_PRINT_SIGFM`)

The driver selects `FPI_PRINT_SIGFM` (OpenCV SIFT keypoints + Lowe ratio test +
RANSAC 2D partial-affine verification) instead of NBIS:

- enrollment stages: `20` (`GM168_NR_ENROLL_STAGES`)
- match threshold (RANSAC inliers): `8` (`GM168_SIGFM_THRESHOLD`)
- enrollment anti-duplicate threshold: `15` (`GM168_SIGFM_DUP_THRESHOLD`, max
  6 consecutive duplicate rejections before safety fallback)
- verify/identify auto-retry count: `2` (`GM168_SIGFM_VERIFY_RETRIES`)
- quality gate: central 48x48 std-dev proxy + dual-capture selection per touch

These parameters were tuned on limited test hardware (`27c6:589a`, firmware
`GFUSB_GM168SEC_APP_10036`) and remain subject to broader validation across
independent sensors.
