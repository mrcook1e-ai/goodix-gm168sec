#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the experimental goodix-gm168 Fedora RPM + SRPM from the current
# working-tree overlay and the pinned pristine libfprint v1.94.100 archive.
#
# usage: build-rpm.sh [output-dir]
#
# The script never touches the host system: all clones, archives and rpmbuild
# phases run inside an isolated mktemp topdir which is removed on exit.
set -euo pipefail

readonly VERSION="0.2.0"
readonly LIBFPRINT_VERSION="1.94.100"
readonly LIBFPRINT_TAG="v${LIBFPRINT_VERSION}"
readonly LIBFPRINT_HEAD="80a4b5ec612892c5c056c48dddaa561452cf37ec"
readonly UPSTREAM_URL="https://gitlab.freedesktop.org/libfprint/libfprint.git"

# Explicit safe overlay paths archived into Source1. Never picks up
# untracked build trees (libfprint-src/, build/, prefix/) or secrets.
readonly -a OVERLAY_PATHS=(
    driver tests patches packaging docs
    .github/workflows/release-rpm.yml
    README.md README.ru.md INSTALL.md NOTICE SECURITY.md CONTRIBUTING.md AGENTS.md LICENSE
    CHANGELOG.md
)

usage() {
    echo "usage: $0 [output-dir]" >&2
    echo "  output-dir  where to place the RPM, SRPM and SHA256SUMS (default: ./dist)" >&2
}

error() {
    echo "build-rpm: error: $*" >&2
    exit 1
}

main() {
    if [ "$#" -gt 1 ]; then
        usage
        exit 64
    fi
    local output_dir="${1:-./dist}"

    # Resolve repository root (packaging/fedora/..) regardless of CWD.
    local repo_root
    repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
    local spec_file="$repo_root/packaging/fedora/libfprint-goodix-gm168.spec"
    [ -f "$spec_file" ] || error "spec not found: $spec_file"

    for tool in git tar bzip2 rpmbuild sha256sum; do
        command -v "$tool" >/dev/null 2>&1 || error "missing required tool: $tool"
    done

    local topdir
    topdir="$(mktemp -d "${TMPDIR:-/tmp}/gm168-rpm.XXXXXX")"
    # Capture the randomized path now: function-local variables are out of
    # scope by the time a process-level EXIT trap runs.
    trap "rm -rf -- $(printf '%q' "$topdir")" EXIT
    trap 'exit 1' HUP INT TERM

    mkdir -p "$topdir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

    # --- Pristine pinned upstream checkout and Source0 archive ---
    echo ">> Cloning pinned libfprint ${LIBFPRINT_TAG}"
    local clone_dir="$topdir/libfprint-src"
    git clone --quiet -c advice.detachedHead=false \
        --branch "$LIBFPRINT_TAG" --depth 1 \
        "$UPSTREAM_URL" "$clone_dir" || error "git clone of $UPSTREAM_URL failed"
    local head
    head="$(git -C "$clone_dir" rev-parse --verify HEAD)"
    if [ "$head" != "$LIBFPRINT_HEAD" ]; then
        error "upstream HEAD ($head) does not match pinned ${LIBFPRINT_TAG} ($LIBFPRINT_HEAD)"
    fi
    # git archive streams tar; compress with bzip2 for the .tar.bz2 Source0.
    git -C "$clone_dir" archive \
        --format=tar \
        --prefix="libfprint-v${LIBFPRINT_VERSION}/" \
        HEAD | bzip2 -c > "$topdir/SOURCES/libfprint-v${LIBFPRINT_VERSION}.tar.bz2" \
        || error "git archive of upstream failed"

    # --- Overlay Source1 archive from explicit safe working-tree paths ---
    echo ">> Staging overlay ${VERSION}"
    local stage="$topdir/overlay/goodix-gm168-${VERSION}"
    mkdir -p "$stage"
    local overlay_list
    overlay_list="$(git -C "$repo_root" ls-files -co --exclude-standard -- "${OVERLAY_PATHS[@]}")" \
        || error "failed to enumerate overlay sources"
    local p
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        # A tracked path may be deleted in the working tree; required build
        # files are checked again by rpmbuild. Ignored secret/blob patterns are
        # never copied into the source archive. Symlinks are rejected so the
        # archive cannot unexpectedly escape its source tree when unpacked.
        [ ! -L "$repo_root/$p" ] || error "symlink is not allowed in overlay: $p"
        [ -f "$repo_root/$p" ] || continue
        mkdir -p "$stage/$(dirname "$p")"
        cp -a "$repo_root/$p" "$stage/$p" || error "failed to copy overlay path: $p"
    done <<< "$overlay_list"
    tar -C "$topdir/overlay" -czf \
        "$topdir/SOURCES/goodix-gm168-${VERSION}.tar.gz" \
        "goodix-gm168-${VERSION}" || error "overlay archive failed"

    # --- rpmbuild ---
    echo ">> rpmbuild -ba"
    rpmbuild -ba \
        --define "_topdir $topdir" \
        --define "_sourcedir $topdir/SOURCES" \
        "$spec_file" || error "rpmbuild failed"

    # --- Collect outputs ---
    mkdir -p "$output_dir"
    local bin_rpm src_rpm
    bin_rpm="$(find "$topdir/RPMS" -type f -name '*.rpm' -print -quit)"
    src_rpm="$(find "$topdir/SRPMS" -type f -name '*.rpm' -print -quit)"
    [ -n "$bin_rpm" ] || error "no binary RPM produced"
    [ -n "$src_rpm" ] || error "no SRPM produced"
    cp -f "$bin_rpm" "$src_rpm" "$output_dir/"

    (
        cd "$output_dir"
        sha256sum "$(basename "$bin_rpm")" "$(basename "$src_rpm")" > SHA256SUMS
    )

    echo ">> Build complete:"
    ls -l "$output_dir"
}

main "$@"
