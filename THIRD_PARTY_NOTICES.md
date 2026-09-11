# Third-party notices

The repository's original code is GPL-3.0-only. Third-party files retain their
own licenses and copyright notices.

| Component | Revision | License | Upstream |
| --- | --- | --- | --- |
| WineCX | `7dbc5b5322a6ef3fb04bdc643c64b188fd641149` | LGPL-2.1-or-later | https://github.com/dappermint/winecx |
| D3D11On12 | `5d8898b43d817e0fd0f58ed87ef645c0cf6fac27` | MIT | https://github.com/microsoft/D3D11On12 |
| D3D12TranslationLayer | `b68ebbc6dab4195f7d4819ea78f2a60c985b853a` | MIT | https://github.com/microsoft/D3D12TranslationLayer |
| DirectX-Headers | `9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e` (`v1.619.1`) | MIT | https://github.com/microsoft/DirectX-Headers |

The Microsoft components' MIT license texts are present as `LICENSE` files in
their pinned submodules. WineCX's LGPL terms are present in its source tree and
must accompany distributed corresponding source.

Apple D3DMetal is proprietary and is not a repository dependency or a
redistributed artifact. Users import their own copy after installing the GPL
runtime.

No Windows SDK or WDK headers may be added to this repository, and
`scripts/package-d3d11on12-source.sh` together with the `git ls-files` gate in
CI enforce that.

The D3D12TranslationLayer build does consume a few of them.
`scripts/prepare-windows-sdk-overlay.sh` fetches the
`microsoft.windows.sdk.cpp` and `microsoft.windows.wdk.x64` NuGet packages,
pinned by SHA-256 and rejected on mismatch, extracts only the headers that
build needs, and writes them to a scratch directory outside the workspace.
They are never committed, never packaged, and never redistributed. The
packages carry Microsoft's own licence terms, which apply to whoever runs the
build.

Missing DDI ABI declarations intended for redistribution must still be
independently authored from public specifications with documented
provenance.
