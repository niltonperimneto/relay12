#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Materialize the SDK/WDK display headers needed by the MinGW DTL and
# D3D11On12 builds.
set -eu

output_dir=${1:?usage: prepare-windows-sdk-overlay.sh OUTPUT_DIR}
version=10.0.26100.1
sdk_sha256=b4730a467a8f29145fc0136b2b3f626767e985f6aa6e32f1352953c38d6ff5d2
wdk_sha256=247b2919ae451f65ba5f1cd51c7c39730fb0fc383d607f3e8ab317fddc8a8239
scratch_dir=$(mktemp -d "${TMPDIR:-/tmp}/relay12-windows-sdk.XXXXXX")
trap 'rm -rf "$scratch_dir"' EXIT HUP INT TERM

download_package() {
    package=$1
    expected=$2
    destination=$3
    url="https://api.nuget.org/v3-flatcontainer/$package/$version/$package.$version.nupkg"
    curl --fail --location --retry 3 --silent --show-error \
        --output "$destination" "$url"
    if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "$destination" | awk '{print $1}')
    else
        actual=$(shasum -a 256 "$destination" | awk '{print $1}')
    fi
    if test "$actual" != "$expected"; then
        echo "SHA-256 mismatch for $package: expected $expected, got $actual" >&2
        exit 1
    fi
}

sdk_package="$scratch_dir/windows-sdk.nupkg"
wdk_package="$scratch_dir/windows-wdk.nupkg"
download_package microsoft.windows.sdk.cpp "$sdk_sha256" "$sdk_package"
download_package microsoft.windows.wdk.x64 "$wdk_sha256" "$wdk_package"

mkdir -p "$scratch_dir/sdk" "$scratch_dir/wdk" "$output_dir"
unzip -q "$sdk_package" -d "$scratch_dir/sdk"
unzip -q "$wdk_package" -d "$scratch_dir/wdk"

# Located by name rather than by absolute path. Hardcoded paths made a missing
# header surface hundreds of lines into a MinGW build as a bare "No such file
# or directory" on an include the overlay was supposed to satisfy; by name, an
# absent or moved header fails here and says which one. It also survives the
# package layout skew, where a 10.0.26100.1 package carries a 10.0.26100.0
# include directory.
copy_header() {
    header=$1
    found=$(find "$scratch_dir/sdk" "$scratch_dir/wdk" -type f -name "$header" \
        | sort | head -n 1)
    if test -z "$found"; then
        echo "$header is not in the pinned SDK/WDK packages" >&2
        exit 1
    fi
    cp "$found" "$output_dir/$header"
}

# Direct DTL requirements not supplied by MinGW-w64 or DirectX-Headers.
# FormatDesc.hpp and DXGIColorSpaceHelper.h deliberately are absent: they are
# DTL-local headers, not Windows SDK files.
copy_header dxva.h
copy_header d3d12TokenizedProgramFormat.hpp

# Direct D3D11On12 display/DDI requirements.
copy_header d3dkmthk.h
copy_header dxgiddi.h
copy_header d3d10umddi.h

# Transitive closure pulled in by the display/DDI headers above. Keeping this
# group explicit makes package drift fail in the overlay preparation step.
copy_header d3dkmddi.h
copy_header d3dkmdt.h
copy_header d3dukmdt.h
copy_header dxmini.h
copy_header d3dumddi.h
copy_header coguid.h

report_optional_header() {
    header=$1
    found=$(find "$scratch_dir/sdk" "$scratch_dir/wdk" -type f -name "$header" \
        | sort | head -n 1)
    if test -n "$found"; then
        cp "$found" "$output_dir/$header"
        echo "overlay: $header is available in the pinned packages"
    else
        echo "overlay: $header is NOT in the pinned packages" >&2
    fi
}

# The shader path's dependencies. Reported, not required: see
# docs/D3D11ON12.md on the DXBC-to-DXIL decision still open.
report_optional_header DxbcConverter.h
report_optional_header dxcapi.h
report_optional_header DxilPipelineStateValidation.h

# Clang and GCC correctly reject the SDK's signed `~0 << bit` enum constant.
# Preserve the mask while making the shift unsigned. Refuse an SDK drift that
# would otherwise turn this into a silent, partial rewrite.
python3 - "$output_dir/dxva.h" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text(encoding="utf-8-sig")
old = "#define DXVABitMask(__n) (~((~0) << __n))"
new = "#define DXVABitMask(__n) (~((~0u) << __n))"
if source.count(old) != 1:
    raise SystemExit("unexpected DXVABitMask declaration in pinned dxva.h")
path.write_text(source.replace(old, new), encoding="utf-8")
PY

# Listed, because "the overlay step succeeded" and "the compiler found the
# header" turned out to be different claims: a build failed on a missing
# include that this script reported having copied.
echo "prepared Windows SDK $version overlay in $output_dir"
ls -l "$output_dir"
