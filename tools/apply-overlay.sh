#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/clean/libfprint-v1.94.100" >&2
    exit 64
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=$1
expected=80a4b5ec612892c5c056c48dddaa561452cf37ec
patch_file="$repo_root/patches/libfprint-v1.94.100-integration.patch"
driver_src="$repo_root/driver"
driver_dst="$target/libfprint/drivers/goodix_gm168"
recovery_test_src="$repo_root/tests/test-goodix-gm168-psk-recovery.c"
recovery_test_dst="$target/tests/test-goodix-gm168-psk-recovery.c"

if [ ! -d "$target" ]; then
    echo "target directory does not exist: $target" >&2
    exit 65
fi
if [ ! -e "$target/.git" ] || ! git -C "$target" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "target must be a clean libfprint v1.94.100 git checkout ($expected)" >&2
    exit 65
fi

head_commit=$(git -C "$target" rev-parse --verify HEAD 2>/dev/null || true)
if [ "$head_commit" != "$expected" ]; then
    echo "target HEAD ($head_commit) does not match pinned libfprint v1.94.100 ($expected)" >&2
    exit 65
fi
if [ -n "$(git -C "$target" status --porcelain --untracked-files=all)" ]; then
    echo "target checkout has local changes or untracked files; refusing to modify it" >&2
    exit 65
fi
if [ ! -f "$patch_file" ] || [ ! -d "$driver_src" ] ||
   [ ! -f "$recovery_test_src" ]; then
    echo "overlay files missing under $repo_root" >&2
    exit 66
fi
if [ -e "$driver_dst" ] || [ -e "$recovery_test_dst" ]; then
    echo "driver or recovery-test destination already exists" >&2
    exit 65
fi

git -C "$target" apply --check "$patch_file"
install -d "$driver_dst"
install -m 0644 "$driver_src"/* "$driver_dst/"
install -m 0644 "$recovery_test_src" "$recovery_test_dst"
git -C "$target" apply "$patch_file"

echo "GM168 overlay applied to $target"
