# Installing and Building `goodix-gm168sec`

This guide covers building the `goodix_gm168` overlay against the pinned
`libfprint` base (`v1.94.100`, commit
`80a4b5ec612892c5c056c48dddaa561452cf37ec`).

Do **not** copy built libraries over `/usr/lib*` behind your package manager,
and do **not** add global `/etc/ld.so.conf.d/` overrides. Start with an isolated
build prefix and run the unit tests first.

## Prerequisites

### Build and test requirements

- `git`
- `meson >= 0.62.0`, `ninja-build`, `pkgconf-pkg-config`
- `gcc` and `gcc-c++` (C11 driver + C++17 SIGFM module)
- `glib-2.0 >= 2.68`, `gio-unix-2.0`, `gobject-2.0`, `gmodule-2.0`
- `gusb >= 0.2.0`
- **OpenSSL 3 development headers (`openssl >= 3.0, < 4.0`)**:
  - **Fedora 45**: `openssl3-devel` (default `openssl-devel` on Fedora 45 ships
    OpenSSL 4, which is rejected by the `goodix_gm168` build check)
  - **Fedora 44 / older**: `openssl-devel`
  - **Arch Linux**: `openssl`
- **OpenCV 4 (`opencv4 >= 4.5.0`)** with core, features2d, and calib3d
- **Optional unit-test headers**: `doctest` (`>= 2.0.0, < 3.0.0`) to build
  `sigfm-tests`, `cairo` for `fpi-assembling` test

### Fedora 45 package installation

```bash
sudo dnf install \
  git meson ninja-build pkgconf-pkg-config gcc gcc-c++ \
  glib2-devel libgusb-devel openssl3-devel \
  opencv-devel doctest-devel cairo-devel
```

Confirm pkg-config sees OpenSSL 3 and OpenCV 4:

```bash
pkg-config --modversion openssl   # must be >= 3.0 and < 4.0
pkg-config --modversion opencv4   # must be >= 4.5.0
```

## 1. Clone pinned `libfprint` and apply the overlay

```bash
git clone --branch v1.94.100 --depth 1 \
  https://gitlab.freedesktop.org/libfprint/libfprint.git

./tools/apply-overlay.sh ./libfprint
```

`tools/apply-overlay.sh` verifies that `./libfprint` is a clean git checkout at
`80a4b5ec612892c5c056c48dddaa561452cf37ec`, copies `driver/*` into
`libfprint/libfprint/drivers/goodix_gm168/` and the synthetic recovery test into
`libfprint/tests/test-goodix-gm168-psk-recovery.c`, checks the integration patch with
`git apply --check`, and applies `patches/libfprint-v1.94.100-integration.patch`.

## 2. Configure, compile, and run tests

Configure an isolated build tree targeting `goodix_gm168`:

```bash
meson setup build ./libfprint \
  --prefix="$PWD/prefix" \
  -Ddrivers=goodix_gm168 \
  -Dgtk-examples=false \
  -Ddoc=false \
  -Dinstalled-tests=false

meson compile -C build
meson test -C build --print-errorlogs
```

When `doctest` and `cairo` are installed, `meson test` executes the SIGFM C++
unit tests (`sigfm-tests`), libfprint SIGFM core integration tests (`fpi-sigfm`),
`fpi-device`, `fpi-ssm`, and `fpi-assembling` using synthetic fixtures (no USB
hardware required).

You can also run the standalone timeout contract check directly from this repo:

```bash
cc -std=c11 -Wall -Wextra -Werror -Idriver tests/test_timeouts.c -o /tmp/test_timeouts
/tmp/test_timeouts
rm -f /tmp/test_timeouts
```

To install into the isolated staging prefix (not system `/usr`):

```bash
meson install -C build
```

## 3. Udev rule (optional, for non-root CLI / example testing)

To grant the active logged-in user `uaccess` permissions for `27c6:589a`:

```bash
sudo install -D -m 0644 packaging/udev/70-goodix-gm168.rules \
  /etc/udev/rules.d/70-goodix-gm168.rules
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=27c6
```

## 4. Per-device PSK loading and recovery

If `psk.bin` is absent, activation attempts bounded read-only recovery of the
existing sensor key and uses it in memory for TLS. No Windows unseal step,
sensor provisioning, or host-file write is required. This is currently
validated only with `GFUSB_GM168SEC_APP_10036`.

### Optional persistent PSK for system `fprintd`

If you already have a backup of your own key, install it privately:

```bash
sudo install -d -m 0700 -o root -g root /etc/goodix-gm168
sudo install -m 0600 -o root -g root /path/to/your/psk.bin /etc/goodix-gm168/psk.bin
```

The driver verifies the file against the sensor hash before TLS. An unreadable,
malformed, or mismatching existing file fails closed; move a known-stale backup
aside deliberately rather than letting the driver overwrite it. PSK recovery
itself needs no `ReadWritePaths` exception. The separate optional
`fdt_thresholds.bin` state write may still need one under a restrictive systemd
sandbox.

### For isolated non-root testing

Point `GOODIX_GM168_DIR` at a private directory owned by your user (`0700`). If
you have a persistent key backup, place `psk.bin` there with mode `0600`:

```bash
install -d -m 0700 "$PWD/device-state"
install -m 0600 /path/to/your/psk.bin "$PWD/device-state/psk.bin"
export GOODIX_GM168_DIR="$PWD/device-state"
```

### Important notes on PSK and calibration

- **If `psk.bin` is missing**, activation reads a fresh PSK hash, walks at most
  4096 bytes in at most 16 forward-only `0xE4` requests, parses the DPAPI v1
  length dynamically, and accepts only an exact 104-byte `0xbb010003` record.
  The recovered PSK must match the fresh hash and remains in memory only.
- **Recovery is not provisioning**: it never writes or rotates the sensor key,
  never saves `psk.bin`/`sealed.bin`, and never runs as a retry after a TLS
  failure. Keep a secure PSK backup to avoid repeating recovery after a process
  restart.
- **Re-provisioning under Windows** can rotate the PSK stored in the sensor,
  invalidating an older `psk.bin`.
- **Calibration files are optional**: the driver performs a 5-frame dynamic dark
  background capture at initialization and works without `calib_windows.dat` or
  `cal_scale.bin`. If you supply matching local calibration files, place them in
  the same private `0700` directory with mode `0600`, and never share or commit
  them.

## Fedora RPM spec (experimental / unsupported)

An experimental RPM spec lives at `packaging/fedora/libfprint-goodix-gm168.spec`.
Because it declares `Conflicts: libfprint` and replaces the system `libfprint`
package, treat it as **unsupported / experimental** for packaging development
only; prefer isolated `meson` builds unless you are testing in a disposable
build environment.
