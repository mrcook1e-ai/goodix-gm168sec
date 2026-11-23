# Frequently Asked Questions (FAQ)

## PSK Management and Recovery

### What happens if `/etc/goodix-gm168/psk.bin` is missing?

When `psk.bin` returns `ENOENT` during `dev_activate()`, the driver automatically runs bounded, read-only existing-key recovery in `INIT` (`driver/goodix_gm168.c` + `driver/gm168_psk_recovery.c`):

1. Queries the fresh PSK SHA-256 via `0xE4` tag `0xbb020001` and rejects zero sentinels.
2. Reads the `0xbb010002` area prefix forward-only (at most `16` chunks of `256` bytes, max `4096` bytes).
3. Dynamically parses the DPAPI v1 length, validates the adjacent `104`-byte `0xbb010003` white-box record, decrypts it in memory using OpenSSL 3, and verifies `SHA256(recovered) == expected_hash` in constant time.
4. Uses the recovered 32-byte key **in memory only** for the TLS 1.2 handshake.

It does **not** create `/etc/goodix-gm168/psk.bin` or write `sealed.bin` automatically. See [`PSK-RECOVERY.md`](PSK-RECOVERY.md).

### What happens if `psk.bin` is stale, corrupted, wrong size, or mismatches the sensor?

The driver **fails closed**:

- **Unreadable (non-`ENOENT`) or wrong size (`!= 32` bytes)**: fails immediately in `dev_activate()` before USB reset or `INIT`.
- **Valid `32` bytes whose SHA-256 mismatches the sensor's `0xbb020001` digest**: fails in `INIT_PSK_HASH_RECV` (`FP_DEVICE_ERROR_PROTO`).

The driver never silently ignores or overwrites an existing `psk.bin`. If you know your backup is stale (for example, after Windows rotated the sensor key), back it up or move it aside deliberately so `psk.bin` is absent (`ENOENT`) and in-memory recovery can run.

### What happens if I dual-boot Windows and re-enroll or re-provision the reader?

The 32-byte PSK is **not** an immutable factory secret. During normal Windows operation, if the Windows driver can unseal its DPAPI copy (`0xbb010002`) and its SHA-256 matches `0xbb020001`, it leaves the key untouched. However, if Windows encounters an invalid or missing DPAPI copy (such as after a Windows reinstall or driver re-provisioning), it erases the application firmware (`0xA4`), enters IAP mode, generates a **new** random 32-byte PSK, writes new `0xE0` records, and re-flashes the firmware.

After any Windows key rotation, an older Linux `psk.bin` will mismatch the sensor and fail activation until moved aside or updated with a fresh private backup. See [`REVERSE-ENGINEERING.md`](REVERSE-ENGINEERING.md).

---

## Calibration and Hardware Behavior

### Are `calib_windows.dat` and `cal_scale.bin` required?

No. Both files are **optional**. By default, the driver captures `5` dark frames during `INIT`, validates that no finger is resting on the sensor (`GM168_BG_MIN_MEAN = 2200`), and runs dynamic background subtraction, 3×3 median filtering, 11-pixel envelope contrast stretching, and `2x` bilinear upscaling to `160x128`.

Do **not** fabricate synthetic calibration files or distribute per-device calibration blobs from another laptop. See [`IMAGE-PIPELINE.md`](IMAGE-PIPELINE.md).

### What firmware versions are supported?

Only **`GFUSB_GM168SEC_APP_10036`** on USB `27c6:589a` is currently confirmed on physical hardware. Earlier code comments referencing `GF3206_GM168SEC_APP_10008` belong to legacy development hardware, and alternative community firmware variants (such as `GFUSB_GM168SEC_APP_10019`) proved incompatible with this capture pipeline and are unsupported. Do **not** attempt experimental firmware flashing on working devices.

---

## Fedora 45, OpenSSL 3, and Matching

### Why does building fail on Fedora 45 with OpenSSL 4, or why does TLS fail at runtime?

There are two separate OpenSSL 3 requirements on Fedora 45:

1. **Build time (`openssl >= 3.0, < 4.0`)**:
   - `meson.build` explicitly rejects OpenSSL `4.x` when `goodix_gm168` is enabled. On Fedora 45, the default `openssl-devel` package ships OpenSSL 4 headers; swap to OpenSSL 3 development headers instead (`sudo dnf install openssl3-devel`, or `sudo dnf swap openssl-devel openssl3-devel` if OpenSSL 4 headers are already installed—OpenSSL 4 runtime libraries remain untouched). See [`INSTALL.md`](../INSTALL.md).
2. **Runtime (`Provider routines::cipher operation failed` / `decryption failed or bad record mac`)**:
   - Even when built against OpenSSL 3, the sensor speaks legacy TLS 1.2 `PSK-AES128-CBC-SHA256` with MAC-then-Encrypt. Under Fedora 45's default OpenSSL configuration, `SSL_accept` may fail unless `fprintd` is given OpenSSL 3's legacy-compatible config via a systemd drop-in (`/etc/systemd/system/fprintd.service.d/goodix-gm168.conf`):
     ```ini
     [Service]
     Environment=OPENSSL_CONF=/etc/pki/tls/openssl3.cnf
     ```
   - Scope this strictly to `fprintd.service` so system-wide `crypto-policies` remain untouched.

### Why did my old fingerprints stop working after switching to this SIGFM build?

This overlay switches `goodix_gm168` from NBIS (`FPI_PRINT_NBIS`) to SIGFM (`FPI_PRINT_SIGFM`). Old NBIS templates stored in `/var/lib/fprint/` are incompatible with SIGFM and are safely rejected by `fp_print_compatible()`.

Delete your existing templates (`fprintd-delete <user>`) and re-enroll (`20` stages). During enrollment, lift completely between touches and shift your finger slightly across zones—the anti-duplicate filter (`GM168_SIGFM_DUP_THRESHOLD = 15`) rejects identical repeated placements to maximize coverage across the small `4 × 3.2 mm` capacitive pad. Existing **SGFM v1** templates remain format-compatible with the reciprocal-matching update; no format migration is implied. See [`SIGFM.md`](SIGFM.md).

### Why does it ask me to center my finger instead of reporting a match result?

The revised SIGFM gate rejects frames with fewer than `30` keypoints or insufficient ridge-like `8x8`-tile coverage. During enrollment this prevents a poor stage from entering the gallery. During verification, a low-coverage frame is still allowed to match; only a no-match becomes `FP_DEVICE_RETRY_CENTER_FINGER`. Use a firm, centered press and lift fully between attempts. These quality retries are not anti-spoofing decisions or calibrated FAR/FRR measurements.

---

## Bug Reports and Security

### What should I attach (and NOT attach) to a GitHub issue?

- **Safe to include**:
  - Laptop make and model;
  - `lsusb` line showing `27c6:589a` (redact any serial numbers);
  - Firmware version string logged by the driver (e.g., `GFUSB_GM168SEC_APP_10036`);
  - Distribution, kernel, `libfprint`, `fprintd`, OpenSSL, and OpenCV versions;
  - State machine name and numeric state / sanitized error message (with personal home paths removed).
- **NEVER attach or commit**:
  - `psk.bin`, PSK SHA-256 hashes, `sealed.bin`, or Windows DPAPI blobs;
  - `calib_windows.dat`, `cal_scale.bin`, or `fdt_thresholds.bin`;
  - Raw fingerprint images (`.pgm`, `.png`, `.raw`, including `GM168_DUMP_FRAMES` output) or `/var/lib/fprint/` templates;
  - `GM168_LOG_DIR` session logs, `.pcapng` USB captures, VM images, firmware binaries, or proprietary Windows driver files.

See [`CONTRIBUTING.md`](../CONTRIBUTING.md) and [`SECURITY.md`](../SECURITY.md) before opening an issue or security advisory.
