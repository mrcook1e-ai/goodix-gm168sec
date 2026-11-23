%global libfprint_version 1.94.100

Name:           libfprint-goodix-gm168
Version:        0.2.0
Release:        0.1%{?dist}
Summary:        Experimental libfprint with out-of-tree Goodix GM168SEC SIGFM driver
License:        LGPL-2.1-or-later
URL:            https://github.com/mrcook1e-ai/goodix-gm168sec

# Experimental / unsupported community packaging spec.
# Track the exact upstream base commit the overlay patch targets.
Source0:        https://gitlab.freedesktop.org/libfprint/libfprint/-/archive/v%{libfprint_version}/libfprint-v%{libfprint_version}.tar.bz2
Source1:        goodix-gm168-%{version}.tar.gz

BuildRequires:  meson >= 0.62.0
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  patch
BuildRequires:  pkgconfig(glib-2.0) >= 2.68
BuildRequires:  pkgconfig(gio-2.0) >= 2.68
BuildRequires:  pkgconfig(gio-unix-2.0) >= 2.68
BuildRequires:  pkgconfig(gobject-2.0) >= 2.68
BuildRequires:  pkgconfig(gmodule-2.0) >= 2.68
BuildRequires:  pkgconfig(gusb) >= 0.2.0
BuildRequires:  (pkgconfig(openssl) >= 3.0 with pkgconfig(openssl) < 4.0)
BuildConflicts: pkgconfig(openssl) >= 4.0
BuildRequires:  pkgconfig(opencv4) >= 4.5.0
BuildRequires:  (pkgconfig(doctest) >= 2.0.0 with pkgconfig(doctest) < 3.0.0)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(gobject-introspection-1.0)
BuildRequires:  systemd-rpm-macros
BuildRequires:  systemd-udev

Requires:       fprintd
Requires:       (openssl3-libs%{?_isa} if openssl-libs%{?_isa} >= 1:4.0)

# Replaces system libfprint in %%{_libdir}; do not ship a global ldconfig override.
Conflicts:      libfprint
Conflicts:      libfprint-devel
Provides:       libfprint = %{libfprint_version}
Provides:       libfprint%{?_isa} = %{libfprint_version}
Provides:       libfprint-devel = %{libfprint_version}
Provides:       libfprint-devel%{?_isa} = %{libfprint_version}

%description
Experimental community developer preview of libfprint %{libfprint_version}
with the Goodix GM168SEC fingerprint driver (USB 27c6:589a) and SIGFM
matching support (FPI_PRINT_SIGFM).

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
%meson \
    -Ddrivers=goodix_gm168 \
    -Dudev_rules=disabled \
    -Dudev_hwdb=disabled \
    -Dgtk-examples=false \
    -Ddoc=false \
    -Dinstalled-tests=false \
    %{nil}
%meson_build

%check
%meson_test --print-errorlogs
%{__cc} %{optflags} -std=c11 -Wall -Wextra -Werror \
    -Ilibfprint/drivers/goodix_gm168 \
    goodix-gm168-%{version}/tests/test_timeouts.c \
    -o test_timeouts_check
./test_timeouts_check
rm -f test_timeouts_check

%install
%meson_install

install -d -m 0700 %{buildroot}%{_sysconfdir}/goodix-gm168
install -D -m 0644 \
    goodix-gm168-%{version}/packaging/udev/70-goodix-gm168.rules \
    %{buildroot}%{_udevrulesdir}/70-goodix-gm168.rules

install -D -m 0644 goodix-gm168-%{version}/README.md        %{buildroot}%{_docdir}/%{name}/README.md
install -D -m 0644 goodix-gm168-%{version}/INSTALL.md       %{buildroot}%{_docdir}/%{name}/INSTALL.md
install -D -m 0644 goodix-gm168-%{version}/NOTICE           %{buildroot}%{_docdir}/%{name}/NOTICE
install -D -m 0644 goodix-gm168-%{version}/SECURITY.md      %{buildroot}%{_docdir}/%{name}/SECURITY.md
install -d %{buildroot}%{_docdir}/%{name}/docs
install -m 0644 goodix-gm168-%{version}/docs/*.md %{buildroot}%{_docdir}/%{name}/docs/

%post
%udev_rules_update
if systemctl is-active --quiet fprintd >/dev/null 2>&1; then
    systemctl restart fprintd >/dev/null 2>&1 || :
fi

%postun
%udev_rules_update
if [ $1 -eq 0 ] && systemctl is-active --quiet fprintd >/dev/null 2>&1; then
    systemctl restart fprintd >/dev/null 2>&1 || :
fi

%files
%license COPYING
%doc %{_docdir}/%{name}
%{_libdir}/libfprint-2.so
%{_libdir}/libfprint-2.so.2*
%{_libdir}/pkgconfig/libfprint-2.pc
%{_includedir}/libfprint-2/
%{_libdir}/girepository-1.0/FPrint-2.0.typelib
%{_datadir}/gir-1.0/FPrint-2.0.gir
%{_datadir}/metainfo/org.freedesktop.libfprint.metainfo.xml
%{_udevrulesdir}/70-goodix-gm168.rules
%dir %attr(0700,root,root) %{_sysconfdir}/goodix-gm168

%changelog
* Mon Nov 23 2026 mrcook1e <noreply@github.com> - 0.2.0-0.1
- Rebase overlay spec onto libfprint 1.94.100 with SIGFM and OpenSSL 3 policy.
- Use driver/ layout, full integration patch, opencv4/doctest/gcc-c++ deps,
  and 0700 private state directory; drop Windows unseal helper.
