<h1 align="center">goodix-gm168sec</h1>

<p align="center">
  <strong>Goodix GM168SEC · Linux · libfprint</strong><br>
  A public source overlay for the <code>27c6:589a</code> capacitive fingerprint reader.
</p>

<p align="center">
  <a href="https://readme-typing-svg.demolab.com">
    <img src="https://readme-typing-svg.demolab.com/?font=Fira+Code&amp;size=18&amp;pause=1200&amp;color=3B82F6&amp;center=true&amp;vCenter=true&amp;width=720&amp;lines=USB+protocol+and+Binary+Ninja;TLS-PSK+and+read-only+recovery;SIFT+%2B+RANSAC+with+SIGFM" alt="USB protocol, TLS-PSK recovery, and SIGFM matching">
  </a>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-LGPL--2.1--or--later-blue" alt="License: LGPL-2.1-or-later"></a>
  <a href="patches/libfprint-v1.94.100-integration.patch"><img src="https://img.shields.io/badge/libfprint-v1.94.100-4b5563" alt="libfprint v1.94.100"></a>
  <a href="INSTALL.md"><img src="https://img.shields.io/badge/OpenSSL-3.x-721412?logo=openssl" alt="OpenSSL 3"></a>
  <a href="INSTALL.md"><img src="https://img.shields.io/badge/Fedora-45_tested-51a2da?logo=fedora&amp;logoColor=white" alt="Fedora 45 tested"></a>
  <a href="#status"><img src="https://img.shields.io/badge/status-developer_preview-orange" alt="Status: developer preview"></a>
  <a href="https://deepwiki.com/mrcook1e-ai/goodix-gm168sec"><img src="https://deepwiki.com/badge.svg" alt="DeepWiki documentation"></a>
</p>

<p align="center">
  <a href="docs/README.md"><strong>Documentation library</strong></a> ·
  <a href="https://deepwiki.com/mrcook1e-ai/goodix-gm168sec">DeepWiki</a> ·
  <a href="#quick-start-isolated-build--tests">Build &amp; test</a> ·
  <a href="CONTRIBUTING.md">Contribute</a>
</p>

Experimental [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
driver for the **Goodix GM168SEC** capacitive fingerprint sensor, with SIGFM
matching and read-only in-memory PSK recovery.

## Status

**Community developer preview — not production-ready and not upstream-ready.**

- **Confirmed hardware / firmware**: USB `27c6:589a` with firmware
  `GFUSB_GM168SEC_APP_10036` (the only currently confirmed firmware revision).
- **Platform tested**: Fedora 45 with OpenSSL 3 (`openssl3-devel` /
  `openssl >= 3.0, < 4.0`).
- **Matching pipeline**: `FPI_PRINT_SIGFM` (OpenCV SIFT + RANSAC affine check)
  instead of NBIS, using 20 enrollment stages with anti-duplicate filtering,
  bounded verify auto-retry, and a 2x bilinear upscale to `160x128`.
- **Background & calibration**: dynamic 5-frame dark background capture and
  subtraction works out of the box without per-device calibration files.
  Optional per-device Windows calibration files (`calib_windows.dat`,
  `cal_scale.bin`) are supported if provided locally, but are never required or
  distributed.
- **Limitations**: evaluated only on limited hardware and a small sample set for
  false accept / false reject behavior. Cross-device compatibility, other
  firmware revisions, hotplug behavior, and suspend/resume lifecycle are not
  broadly validated and should not be assumed production-ready. Always keep a
  password authentication fallback configured.

## AI-assisted development and "as is" notice

Substantial portions of the reverse engineering, implementation, testing, and
documentation in this project were produced with AI coding assistance under
human review and direction. The code has been exercised on limited test hardware
and has not undergone an independent security or correctness audit.

This project is provided **"as is", without warranty of any kind** under the
GNU Lesser General Public License v2.1 or later. See [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE).

## Repository layout

- `driver/` — libfprint `goodix_gm168` driver C sources and headers
- `patches/` — integration patch for pinned libfprint `v1.94.100` (SIGFM core,
  tests, OpenSSL 3 build policy, and udev/hwdb whitelist fix)
- `tools/apply-overlay.sh` — verifies a clean pinned `libfprint v1.94.100`
  checkout, copies the driver and synthetic recovery test, and applies the patch
- `tests/` — timeout contract and synthetic DPAPI/TLV/OpenSSL recovery tests
- `packaging/` — udev `uaccess` rule and experimental Fedora RPM spec
- `docs/` — curated architecture, protocol, RE, SIGFM, recovery, and testing guides

No proprietary binaries, firmware images, Windows driver files, per-device PSKs,
calibration tables, biometric samples, or packet captures are included.

## Quick start (isolated build & tests)

See [`INSTALL.md`](INSTALL.md) for full prerequisites and details.

```bash
git clone --branch v1.94.100 --depth 1 \
  https://gitlab.freedesktop.org/libfprint/libfprint.git

./tools/apply-overlay.sh ./libfprint

meson setup build ./libfprint \
  --prefix="$PWD/prefix" \
  -Ddrivers=goodix_gm168 \
  -Dgtk-examples=false \
  -Ddoc=false \
  -Dinstalled-tests=false

meson compile -C build
meson test -C build --print-errorlogs
```

`tools/apply-overlay.sh` refuses a dirty checkout or any commit other than
`80a4b5ec612892c5c056c48dddaa561452cf37ec` (`v1.94.100`).

## PSK loading and read-only recovery

The sensor enforces a TLS 1.2 PSK handshake (`PSK-AES128-CBC-SHA256`) using a
**device-specific** 32-byte key provisioned on the sensor.

- If `psk.bin` exists, the driver reads it from `/etc/goodix-gm168/psk.bin` or
  `$GOODIX_GM168_DIR/psk.bin` and verifies its SHA-256 against the sensor before
  starting TLS. Keep the file at mode `0600` in a private `0700` directory.
- If the file is absent, the driver performs bounded, read-only recovery of the
  existing key: it reads a fresh sensor hash, discovers the DPAPI v1 record
  length dynamically, decrypts the adjacent white-box record, and verifies the
  recovered key against that fresh hash before starting TLS. This path is
  currently validated only for `GFUSB_GM168SEC_APP_10036`.
- Recovery uses the key **in memory only**. It does not write `psk.bin` or
  `sealed.bin`, provision a new key, rotate the sensor key, or retry after a
  malformed response, hash mismatch, or TLS failure.
- An existing unreadable, malformed, or mismatching `psk.bin` fails activation;
  it is not silently ignored or overwritten. Move a known-stale file aside
  deliberately if you want to retry recovery from the sensor.
- Re-provisioning the reader under Windows can change the device PSK. Back up
  your own `psk.bin` securely and **never** commit or attach PSKs, hashes, DPAPI
  data, or calibration files to public issues.

## Documentation library: meet the sensor, layer by layer

Start with the **[documentation hub](docs/README.md)** or continue to the
**[DeepWiki guide](https://deepwiki.com/mrcook1e-ai/goodix-gm168sec)**. The library
is organized as a learning path rather than a collection of disconnected notes.

| Step | Read | What you will learn |
|---:|---|---|
| 1 | [**Reverse engineering**](docs/REVERSE-ENGINEERING.md) | How Binary Ninja, dynamic analysis, USB behavior, and synthetic differential tests established the protocol without shipping proprietary artifacts. |
| 2 | [**Architecture**](docs/ARCHITECTURE.md) · [**State machines**](docs/STATE-MACHINES.md) | How asynchronous USB, `FpiSsm`, the OpenSSL thread, cancellation, deadlines, and the TLS fast path fit together. |
| 3 | [**USB & TLS protocol**](docs/PROTOCOL.md) | What `A0`/`B0`/`B2` frames carry, how ACK flags work, and why the host runs a legacy TLS-PSK server. |
| 4 | [**Image pipeline**](docs/IMAGE-PIPELINE.md) | How packed 12-bit sensor rows become an `80x64` frame, then a calibrated/background-corrected `160x128` image. |
| 5 | [**SIGFM matching**](docs/SIGFM.md) | Why SIFT + RANSAC replaced NBIS, how `SGFM v1` integrates with libfprint, and what the 20 enrollment stages do. |
| 6 | [**PSK recovery**](docs/PSK-RECOVERY.md) | How dynamic DPAPI parsing, white-box decryption, and fresh-hash verification recover an existing key read-only into memory. |
| 7 | [**Testing**](docs/TESTING.md) · [**Development**](docs/DEVELOPMENT.md) | How to reproduce offline proofs, run memory checks, maintain the overlay, and prepare a safe hardware validation. |

For AI-assisted contributions, read [`AGENTS.md`](AGENTS.md); for safe issue data
and human contributions, see [`CONTRIBUTING.md`](CONTRIBUTING.md). Common setup
and dual-boot questions are covered in the [`FAQ`](docs/FAQ.md).

### Hands-on lab — no finger or secret data required

After the isolated build above, run the synthetic recovery and matching suites:

```bash
meson test -C build \
  goodix-gm168-psk-recovery fpi-sigfm sigfm-tests \
  --print-errorlogs
```

This exercises pinned cryptographic vectors, malformed DPAPI/TLV inputs, SIGFM
serialization/matching, and negative controls without accessing a sensor. See
[`docs/TESTING.md`](docs/TESTING.md) for Valgrind and the offline-vs-live proof
boundary.

## License and provenance

Licensed under **LGPL-2.1-or-later**. See [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE) for full attribution of `mrcook1e-ai`, `wseng`, Daniil
Yakoshinov, the `55b4-experimental` Goodix work, and the SIGFM authors.
