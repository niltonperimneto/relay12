#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Validate and prepare the pinned D3D12TranslationLayer portability source.
set -eu

source_dir=${1:-third_party/D3D12TranslationLayer}
repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
expected_revision=b68ebbc6dab4195f7d4819ea78f2a60c985b853a

test -d "$source_dir"
actual_revision=$(git -C "$source_dir" rev-parse HEAD)
if test "$actual_revision" != "$expected_revision"; then
    echo "D3D12TranslationLayer revision mismatch: expected $expected_revision, got $actual_revision" >&2
    exit 1
fi

for patch in "$repository_root"/patches/dtl/*.patch; do
    test -e "$patch" || break
    git -C "$source_dir" apply --check --ignore-space-change \
            --ignore-whitespace "$patch"
    git -C "$source_dir" apply --ignore-space-change --ignore-whitespace \
            "$patch"
done

# These are canonical across the DTL and D3D11On12 ports. Copying after the
# upstream patch series prevents either prepared tree from carrying a fork.
cp "$repository_root/compat/relay_ownership.hpp" \
    "$source_dir/include/relay_ownership.hpp"
cp "$repository_root/compat/relay_atl_compat.hpp" \
    "$source_dir/include/relay_atl_compat.hpp"
cp "$repository_root/compat/relay_hresult_error.hpp" \
    "$source_dir/include/relay_hresult_error.hpp"
cp "$repository_root/compat/relay_tracelogging.hpp" \
    "$source_dir/include/relay_tracelogging.hpp"

python3 "$repository_root/scripts/inventory_dtl_portability.py" \
    "$source_dir" --check "$repository_root/docs/dtl-portability-baseline.json"
echo "prepared D3D12TranslationLayer source at $actual_revision"
