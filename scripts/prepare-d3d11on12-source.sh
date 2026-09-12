#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Apply relay12's reviewable MinGW portability series to the pinned MIT tree.
set -eu

source_dir=${1:-third_party/D3D11On12}
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
expected_revision=5d8898b43d817e0fd0f58ed87ef645c0cf6fac27

test -d "$source_dir"
actual_revision=$(git -C "$source_dir" rev-parse HEAD)
if test "$actual_revision" != "$expected_revision"; then
    echo "D3D11On12 revision mismatch: expected $expected_revision, got $actual_revision" >&2
    exit 1
fi

cp "$repository_root/compat/relay_ownership.hpp" \
    "$source_dir/include/relay_ownership.hpp"
cp "$repository_root/compat/relay_tracelogging.hpp" \
    "$source_dir/include/relay_tracelogging.hpp"
cp "$repository_root/compat/relay_msvc_pragma.hpp" \
    "$source_dir/include/relay_msvc_pragma.hpp"

for patch in "$repository_root"/patches/d3d11on12/*.patch; do
    # The pinned Microsoft tree stores these sources as CRLF. The relay patch
    # series is kept as normal LF text, so whitespace matching is intentional;
    # it does not relax path, context, or hunk-offset validation.
    git -C "$source_dir" apply --check --ignore-space-change \
            --ignore-whitespace "$patch"
    git -C "$source_dir" apply --ignore-space-change --ignore-whitespace \
            "$patch"
done

python3 "$repository_root/scripts/check_d3d11on12_port.py" "$source_dir"

echo "prepared D3D11On12 source at $actual_revision"
