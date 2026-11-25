%global libfprint_version 1.94.100
%global gm168_prefix /opt/libfprint-gm168
%global debug_package %{nil}

# The private /opt tree carries a libfprint-2 SONAME that must NOT be
# advertised as a system provide. Automatic *Requires* (libssl.so.3,
# libopencv*, ...) must stay intact, so only provides are excluded.
%global __provides_exclude_from ^%{gm168_prefix}/

Name:           libfprint-goodix-gm168
Version:        0.2.0
Release:        1%{?dist}
Summary:        Experimental isolated libfprint build with Goodix GM168SEC SIGFM driver
License:        LGPL-2.1-or-later
URL:            https://github.com/mrcook1e-ai/goodix-gm168sec
ExclusiveArch:  x86_64

# Experimental / unsupported community packaging spec.
# Two-source build: pristine pinned upstream v1.94.100 archive (Source0,
# created by packaging/fedora/build-rpm.sh) plus this overlay (Source1).
# The private prefix coexists with the system libfprint package: no
# Conflicts/Provides against libfprint and no global ld.so.conf.
Source0:        libfprint-v%{libfprint_version}.tar.bz2
Source1:        goodix-gm168-%{version}.tar.gz

BuildRequires:  meson >= 0.62.0
BuildRequires:  ninja-build
BuildRequires:  pkgconf-pkg-config
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  patch
BuildRequires:  diffutils
BuildRequires:  glib2-devel
BuildRequires:  libgusb-devel
BuildRequires:  pixman-devel
BuildRequires:  (pkgconfig(openssl) >= 3.0 with pkgconfig(openssl) < 4.0)
BuildConflicts: pkgconfig(openssl) >= 4.0
BuildRequires:  openssl3-devel
BuildRequires:  pkgconfig(opencv4) >= 4.5.0
BuildRequires:  (pkgconfig(doctest) >= 2.0.0 with pkgconfig(doctest) < 3.0.0)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  systemd-devel
BuildRequires:  systemd-rpm-macros
BuildRequires:  systemd-udev
BuildRequires:  redhat-rpm-config
BuildRequires:  rpm-build
BuildRequires:  bzip2

# Runtime: private OpenSSL 3 sonames (libssl.so.3) and the daemon that
# consumes the library via the bundled drop-in.
Requires:       fprintd
Requires:       openssl3-libs%{?_isa}
Requires(post): systemd
Requires(postun): systemd

%description
Experimental community developer preview of libfprint %{libfprint_version}
with the Goodix GM168SEC fingerprint driver (USB 27c6:589a) and SIGFM
matching support (FPI_PRINT_SIGFM).

The build is isolated under %{gm168_prefix} and coexists with the system
libfprint package; a systemd drop-in makes fprintd prefer the private
library via LD_LIBRARY_PATH.

The driver speaks the sensor's TLS 1.2 PSK-AES128-CBC-SHA256 protocol using
OpenSSL 3, captures 80x64 frames with 5-frame dynamic dark-background
correction, and upscales 2x to 160x128 for SIGFM feature extraction.

A matching 32-byte PSK can be loaded from /etc/goodix-gm168/psk.bin or
recovered read-only into memory when that file is absent. The driver does not
provision a new sensor key or persist the recovered key automatically.
See %{_docdir}/%{name}/INSTALL.md and %{_docdir}/%{name}/docs/PSK-RECOVERY.md.

%prep
%setup -q -n libfprint-v%{libfprint_version} -a 1
install -d libfprint/drivers/goodix_gm168
install -m 0644 goodix-gm168-%{version}/driver/* libfprint/drivers/goodix_gm168/
install -m 0644 goodix-gm168-%{version}/tests/test-goodix-gm168-psk-recovery.c tests/
patch -p1 --fuzz=0 -i goodix-gm168-%{version}/patches/libfprint-v1.94.100-integration.patch

%build
# Raw meson (not %%meson): Fedora %%meson leaks /usr prefix/libdir paths.
# %%set_build_flags exports Fedora optflags for the private prefix build.
%set_build_flags
meson setup %{_vpath_builddir} \
    --prefix=%{gm168_prefix} \
    --libdir=lib64 \
    -Ddrivers=goodix_gm168 \
    -Dudev_rules=disabled \
    -Dudev_hwdb=disabled \
    -Dgtk-examples=false \
    -Ddoc=false \
    -Dinstalled-tests=false \
    -Dintrospection=false \
    %{nil}
%meson_build

%check
# Targeted driver, recovery, and SIGFM suites.
meson test -C %{_vpath_builddir} \
    goodix-gm168-psk-recovery fpi-sigfm sigfm-tests goodix-gm168-frame-layout \
    --print-errorlogs
# Full meson suite (all enabled targets must pass).
meson test -C %{_vpath_builddir} --print-errorlogs
# Standalone timeout contract check.
%{__cc} %{optflags} -std=c11 -Wall -Wextra -Werror \
    -Ilibfprint/drivers/goodix_gm168 \
    goodix-gm168-%{version}/tests/test_timeouts.c \
    -o test_timeouts_check
./test_timeouts_check
rm -f test_timeouts_check

%install
%meson_install

# Private runtime only: strip development and desktop-integration artifacts
# (unversioned linker symlink, headers, pkg-config, GIR/typelib, metainfo and
# any other share leftovers) while keeping the versioned library and the
# libfprint-2.so.2 SONAME symlink.
rm -f %{buildroot}%{gm168_prefix}/lib64/libfprint-2.so
rm -rf %{buildroot}%{gm168_prefix}/include
rm -rf %{buildroot}%{gm168_prefix}/lib64/pkgconfig
rm -rf %{buildroot}%{gm168_prefix}/lib64/girepository-1.0
rm -rf %{buildroot}%{gm168_prefix}/share

# Private state directory (0700). Intentionally empty: psk.bin / calibration
# files placed here by the administrator are unowned by the RPM and survive
# package removal.
install -d -m 0700 %{buildroot}%{_sysconfdir}/goodix-gm168

install -D -m 0644 \
    goodix-gm168-%{version}/packaging/udev/70-goodix-gm168.rules \
    %{buildroot}%{_udevrulesdir}/70-goodix-gm168.rules

# fprintd drop-in: switch the daemon to the private library.
install -d %{buildroot}%{_unitdir}/fprintd.service.d
install -m 0644 \
    goodix-gm168-%{version}/packaging/fedora/fprintd-goodix-gm168.conf \
    %{buildroot}%{_unitdir}/fprintd.service.d/60-goodix-gm168-rpm.conf

install -D -m 0644 goodix-gm168-%{version}/README.md        %{buildroot}%{_docdir}/%{name}/README.md
install -D -m 0644 goodix-gm168-%{version}/README.ru.md     %{buildroot}%{_docdir}/%{name}/README.ru.md
install -D -m 0644 goodix-gm168-%{version}/INSTALL.md       %{buildroot}%{_docdir}/%{name}/INSTALL.md
install -D -m 0644 goodix-gm168-%{version}/NOTICE           %{buildroot}%{_docdir}/%{name}/NOTICE
install -D -m 0644 goodix-gm168-%{version}/SECURITY.md      %{buildroot}%{_docdir}/%{name}/SECURITY.md
install -D -m 0644 goodix-gm168-%{version}/CHANGELOG.md     %{buildroot}%{_docdir}/%{name}/CHANGELOG.md
install -d %{buildroot}%{_docdir}/%{name}/docs
install -m 0644 goodix-gm168-%{version}/docs/*.md %{buildroot}%{_docdir}/%{name}/docs/

%post
# Pick up the new udev rule (uaccess tag for 27c6:589a).
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules >/dev/null 2>&1 || :
    udevadm trigger --subsystem-match=usb --attr-match=idVendor=27c6 --attr-match=idProduct=589a >/dev/null 2>&1 || :
fi
# Restore SELinux labels for the private prefix when SELinux tooling exists.
if command -v restorecon >/dev/null 2>&1; then
    restorecon -R %{gm168_prefix} >/dev/null 2>&1 || :
fi
# Load the drop-in and restart an already-running fprintd so it binds the
# private library. Never fail the transaction in offline/chroot contexts.
if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || :
    systemctl try-restart fprintd.service >/dev/null 2>&1 || :
fi

%postun
if [ "$1" -eq 0 ]; then
    # Final removal: drop-in is gone, reload systemd and restart an active
    # fprintd so it falls back to the system libfprint. Do NOT delete
    # /etc/goodix-gm168 contents; RPM leaves unowned files behind.
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules >/dev/null 2>&1 || :
        udevadm trigger --subsystem-match=usb --attr-match=idVendor=27c6 --attr-match=idProduct=589a >/dev/null 2>&1 || :
    fi
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload >/dev/null 2>&1 || :
        systemctl try-restart fprintd.service >/dev/null 2>&1 || :
    fi
else
    # Upgrade: the %%post of the new package already restarted fprintd.
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload >/dev/null 2>&1 || :
    fi
fi

%files
%license COPYING
%doc %{_docdir}/%{name}/
%{_udevrulesdir}/70-goodix-gm168.rules
%dir %{_unitdir}/fprintd.service.d
%{_unitdir}/fprintd.service.d/60-goodix-gm168-rpm.conf
%dir %attr(0700,root,root) %{_sysconfdir}/goodix-gm168
%dir %{gm168_prefix}
%dir %{gm168_prefix}/lib64
%{gm168_prefix}/lib64/libfprint-2.so.2*

%changelog
* Wed Nov 25 2026 mrcook1e <noreply@github.com> - 0.2.0-1
- Isolate the build under /opt/libfprint-gm168 so the system libfprint
  package coexists (drop Conflicts/Provides, private SONAME provides
  exclusion, no global ld.so.conf).
- Add fprintd systemd drop-in (LD_LIBRARY_PATH + OPENSSL_CONF +
  ReadWritePaths=-/etc/goodix-gm168), guarded scriptlets with
  daemon-reload/try-restart, restorecon and udev reload.
- Build with raw meson + %%set_build_flags; disable introspection; run
  targeted + full test suites and the standalone timeout check in %%check.

* Mon Nov 23 2026 mrcook1e <noreply@github.com> - 0.2.0-0.1
- Rebase overlay spec onto libfprint 1.94.100 with SIGFM and OpenSSL 3 policy.
- Use driver/ layout, full integration patch, opencv4/doctest/gcc-c++ deps,
  and 0700 private state directory; drop Windows unseal helper.
