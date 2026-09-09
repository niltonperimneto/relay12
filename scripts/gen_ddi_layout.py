#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# An independent layout model of the clean-room DDI declaration groups.
#
# The model is written from the same public specifications as
# relay12-d3d11/ddi/wine_d3d11ddi.h, but it derives the offsets by walking member
# lists rather than by asserting numbers, so it is a second opinion about the
# same facts.  It does two jobs.
#
#   --check  compares the model against the committed header: every field the
#            model knows must carry a WINE_DDI_ASSERT_FIELD at the modelled
#            offset, no assertion may name a field the model does not know, and
#            the declared union arm sets must agree with the published ones on
#            every offset and size.  This runs in CI.
#
#   --emit   prints the declarations and their assertion blocks, for authoring
#            a new group.  What is generated is the committed source, not a
#            build step: the header stays a reviewable artifact, and this
#            output is a patch to read before it lands.
#
# Neither mode reads a WDK header, and the model carries no value that is not
# in the cited specification.  Provenance blocks stay hand-written: they carry
# judgment about what was quoted, what was derived and what is missing, and
# that is not mechanizable.
#
# Usage:
#   python3 scripts/gen_ddi_layout.py --check
#   python3 scripts/gen_ddi_layout.py --emit [STRUCT ...]

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass, field as dataclass_field

HEADER = pathlib.Path("relay12-d3d11/ddi/wine_d3d11ddi.h")

# The frozen contract is Win64 x86_64 at natural alignment.  Every type in
# these groups is a pointer, a handle wrapping one, or a 4-byte integer.
POINTER = (8, 8)
UINT = (4, 4)


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


@dataclass
class Field:
    """One named member."""

    name: str
    type_name: str
    size: int = POINTER[0]
    align: int = POINTER[1]


@dataclass
class Union:
    """An anonymous union.

    `arms` are the arms the header declares, which are the ones the pinned
    D3D11On12 source reads.  `published` is every arm the specification's
    syntax block prints.  Holding both is the point: the arms alias, so the
    two sets must agree on every offset and on the enclosing structure's size,
    and that is what makes declaring a subset a declaration choice rather than
    a layout change.
    """

    arms: list
    published: list

    def members_for(self, published):
        return self.published if published else self.arms


@dataclass
class Embedded:
    """A structure member held by value, not behind a pointer."""

    name: str
    struct: "Struct"


@dataclass
class Struct:
    name: str
    members: list = dataclass_field(default_factory=list)

    def walk(self, published=False):
        """Return (fields, size, align).

        `fields` maps a member name to its offset from this structure's base.
        A union contributes every arm at the union's own offset.  An embedded
        structure contributes its own name and, separately, each of its
        members under a dotted path, because a by-value member silently
        becoming a pointer is exactly the mistake that moves everything after
        it.
        """
        fields, size, alignment, _ = self._walk(published)
        return fields, size, alignment

    def padding(self, published=False):
        """Return [(offset, size)] for every gap the member list implies.

        Derived, never transcribed.  The member lists in this model are the
        ones the specification prints, and no specification names padding, so
        the only honest source for where the gaps are is the alignment
        arithmetic that produces them.  The header names each gap and asserts
        it; check() below compares those names against this, so a pad member
        invented at the wrong offset -- or one the header keeps after the
        member before it changed size -- fails rather than being ratified by
        an identical edit on both sides.
        """
        return self._walk(published)[3]

    def _walk(self, published):
        fields = {}
        gaps = []
        offset = 0
        alignment = 1

        def advance(to):
            """Align up to `to`, recording anything skipped as a gap."""
            nonlocal offset
            aligned = align_up(offset, to)
            if aligned != offset:
                gaps.append((offset, aligned - offset))
            offset = aligned

        for member in self.members:
            if isinstance(member, Field):
                advance(member.align)
                fields[member.name] = offset
                offset += member.size
                alignment = max(alignment, member.align)
            elif isinstance(member, Union):
                arms = member.members_for(published)
                arm_align = max(arm.align for arm in arms)
                arm_size = max(arm.size for arm in arms)
                advance(arm_align)
                for arm in arms:
                    fields[arm.name] = offset
                offset += arm_size
                alignment = max(alignment, arm_align)
            elif isinstance(member, Embedded):
                inner, inner_size, inner_align, inner_gaps = member.struct._walk(
                    published)
                advance(inner_align)
                fields[member.name] = offset
                for inner_name, inner_offset in inner.items():
                    fields[f"{member.name}.{inner_name}"] = offset + inner_offset
                gaps.extend(
                    (offset + gap_offset, gap_size)
                    for gap_offset, gap_size in inner_gaps
                )
                offset += inner_size
                alignment = max(alignment, inner_align)
            else:
                raise TypeError(f"unknown member kind: {member!r}")

        # Trailing padding counts too: it is the one gap a following member
        # cannot reveal, and a structure the host allocates has it on the wire
        # like any other.
        advance(alignment)
        return fields, offset, alignment, gaps


# --- The declaration groups -------------------------------------------------
#
# Group: driver and runtime object handles
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/direct3d-version-10-runtime-and-driver-handles
# Retrieved: 2026-09-06
#
# A handle is one wrapped pointer.  The driver handle points at the
# runtime-allocated private block, so its member is pDrvPrivate; the runtime
# handle carries an opaque runtime value, so its member is handle.

# D3D11DDI_HCOMMANDLIST is a driver handle by the same convention: the
# CommandListExecute page calls it "a handle to the driver's private data for
# the command list". D3D11DDI_HRTCOMMANDLIST is its runtime counterpart.
DRIVER_HANDLES = ["D3D10DDI_HADAPTER", "D3D10DDI_HRESOURCE", "D3D10DDI_HDEVICE",
                  "D3D11DDI_HCOMMANDLIST", "D3D10DDI_HSHADERRESOURCEVIEW",
                  "D3D10DDI_HRENDERTARGETVIEW", "D3D10DDI_HSHADER"]
RUNTIME_HANDLES = [
    "D3D10DDI_HRTADAPTER",
    "D3D10DDI_HRTRESOURCE",
    "D3D10DDI_HRTDEVICE",
    "D3D10DDI_HRTCORELAYER",
    "D3DWDDM2_2DDI_HRTCACHESESSION",
    "D3D11DDI_HRTCOMMANDLIST",
    "D3D10DDI_HRTSHADERRESOURCEVIEW",
    "D3D10DDI_HRTRENDERTARGETVIEW",
    "D3D10DDI_HRTSHADER",
]

HANDLES = [
    Struct(name, [Field("pDrvPrivate", "void *")]) for name in DRIVER_HANDLES
] + [Struct(name, [Field("handle", "void *")]) for name in RUNTIME_HANDLES]


def handle(name):
    return (name, POINTER[0], POINTER[1])


# Group: command-list creation arguments
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_createcommandlist
# Retrieved: 2026-09-08

CREATECOMMANDLIST = Struct(
    "D3D11DDIARG_CREATECOMMANDLIST",
    [Field("hDeferredContext", "D3D10DDI_HDEVICE")],
)

# Group: deferred-context creation and handle sizing
# Specifications: D3D11DDIARG_CREATEDEFERREDCONTEXT,
# D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE, and D3D11DDI_HANDLESIZE
# Retrieved: 2026-09-08

CALCPRIVATEDEFERREDCONTEXTSIZE = Struct(
    "D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE",
    [Field("Flags", "UINT", *UINT)],
)

HANDLESIZE = Struct(
    "D3D11DDI_HANDLESIZE",
    [
        Field("HandleType", "D3D11DDI_HANDLETYPE", *UINT),
        Field("DriverPrivateSize", "SIZE_T"),
    ],
)

CREATEDEFERREDCONTEXT = Struct(
    "D3D11DDIARG_CREATEDEFERREDCONTEXT",
    [
        Union(
            arms=[Field("pWDDM2_6ContextFuncs",
                        "D3DWDDM2_6DDI_DEVICEFUNCS *")],
            published=[
                Field("p11ContextFuncs", "D3D11DDI_DEVICEFUNCS *"),
                Field("p11_1ContextFuncs", "D3D11_1DDI_DEVICEFUNCS *"),
                Field("pWDDM1_3ContextFuncs", "D3DWDDM1_3DDI_DEVICEFUNCS *"),
                Field("pWDDM2_0ContextFuncs", "D3DWDDM2_0DDI_DEVICEFUNCS *"),
                Field("pWDDM2_1ContextFuncs", "D3DWDDM2_1DDI_DEVICEFUNCS *"),
                Field("pWDDM2_2ContextFuncs", "D3DWDDM2_2DDI_DEVICEFUNCS *"),
                Field("pWDDM2_6ContextFuncs", "D3DWDDM2_6DDI_DEVICEFUNCS *"),
            ],
        ),
        Field("hDrvContext", "D3D10DDI_HDEVICE"),
        Field("hRTCoreLayer", "D3D10DDI_HRTCORELAYER"),
        Union(
            arms=[Field("pWDDM2_6UMCallbacks",
                        "const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *")],
            published=[
                Field("p11UMCallbacks",
                      "const D3D11DDI_CORELAYER_DEVICECALLBACKS *"),
                Field("pWDDM2_0UMCallbacks",
                      "const D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS *"),
                Field("pWDDM2_2UMCallbacks",
                      "const D3DWDDM2_2DDI_CORELAYER_DEVICECALLBACKS *"),
                Field("pWDDM2_6UMCallbacks",
                      "const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *"),
            ],
        ),
        Field("Flags", "UINT", *UINT),
    ],
)

# Group: resource readback types
# Specification: D3D10DDI_MAPPED_SUBRESOURCE
# Retrieved: 2026-09-08
MAPPED_SUBRESOURCE = Struct(
    "D3D10DDI_MAPPED_SUBRESOURCE",
    [
        Field("pData", "void *"),
        Field("RowPitch", "UINT", *UINT),
        Field("DepthPitch", "UINT", *UINT),
    ],
)

# Group: resource creation and shared-resource opening arguments
# Specification:
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_createresource
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_createresource
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_openresource
# Retrieved: 2026-09-08

CREATERESOURCE = Struct(
    "D3D10DDIARG_CREATERESOURCE",
    [
        Field("pMipInfoList", "const D3D10DDI_MIPINFO *"),
        Field("pInitialDataUP", "const D3D10_DDIARG_SUBRESOURCE_UP *"),
        Field("ResourceDimension", "D3D10DDIRESOURCE_TYPE", *UINT),
        Field("Usage", "UINT", *UINT),
        Field("BindFlags", "UINT", *UINT),
        Field("MapFlags", "UINT", *UINT),
        Field("MiscFlags", "UINT", *UINT),
        Field("Format", "DXGI_FORMAT", *UINT),
        Field("SampleDesc", "DXGI_SAMPLE_DESC", 8, 4),
        Field("MipLevels", "UINT", *UINT),
        Field("ArraySize", "UINT", *UINT),
        Field("pPrimaryDesc", "DXGI_DDI_PRIMARY_DESC *"),
    ],
)

CREATE11RESOURCE = Struct(
    "D3D11DDIARG_CREATERESOURCE",
    CREATERESOURCE.members
    + [
        Field("ByteStride", "UINT", *UINT),
        Field("DecoderBufferType", "D3D11_1DDI_VIDEO_DECODER_BUFFER_TYPE", *UINT),
        Field("TextureLayout", "D3DWDDM2_0DDI_TEXTURE_LAYOUT", *UINT),
    ],
)

OPENRESOURCE = Struct(
    "D3D10DDIARG_OPENRESOURCE",
    [
        Field("NumAllocations", "UINT", *UINT),
        Union(
            arms=[
                Field("pOpenAllocationInfo", "D3DDDI_OPENALLOCATIONINFO *"),
                Field("pOpenAllocationInfo2", "D3DDDI_OPENALLOCATIONINFO2 *"),
            ],
            published=[
                Field("pOpenAllocationInfo", "D3DDDI_OPENALLOCATIONINFO *"),
                Field("pOpenAllocationInfo2", "D3DDDI_OPENALLOCATIONINFO2 *"),
            ],
        ),
        Field("hKMResource", "D3D10DDI_HKMRESOURCE"),
        Field("pPrivateDriverData", "VOID *"),
        Field("PrivateDriverDataSize", "UINT", *UINT),
    ],
)

# Group: shader resource view and render target view creation arguments
# Specification:
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_0ddiarg_createshaderresourceview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_buffer_shaderresourceview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex1d_shaderresourceview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_0ddiarg_tex2d_shaderresourceview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex3d_shaderresourceview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10_1ddiarg_texcube_shaderresourceview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_bufferex_shaderresourceview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_createrendertargetview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_buffer_rendertargetview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex1d_rendertargetview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex2d_rendertargetview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex3d_rendertargetview
#   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_texcube_rendertargetview
# Retrieved: 2026-09-08
#
# hDrvResource is a real D3D11On12 (MIT) parameter name, but its type on the
# ABI boundary is one of this project's own handle wrappers; that handle
# group is declared alongside these structures rather than with the resource
# group, because this is the first place its layout is load-bearing.
#
# D3D10DDIARG_CREATERENDERTARGETVIEW never received a published WDDM 2.0-named
# revision the way the SRV and UAV structures did: the pinned driver's own
# signature (view.cpp) names its argument D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW,
# but both documentation surfaces 404 for that exact name, and every field the
# driver reads through it -- including TexCube.ArraySize/FirstArraySlice, which
# would have forced a revision had the base struct lacked them -- is already
# present on the published D3D10DDIARG_CREATERENDERTARGETVIEW and its arms. The
# local name follows the ABI call site; the fields are the cross-validated
# public ones, unchanged.
#
# The SRV TexCube arm has a genuine surface disagreement: the rendered syntax
# block for D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW types the arm
# D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW (4 fields, with array support), but
# the markdown mirror's prose links the older, 2-field D3D10DDIARG_TEXCUBE_SHADERRESOURCEVIEW
# instead. The pinned driver's GetTranslationDesc reads pDDIDesc->TexCube.First2DArrayFace
# and ->NumCubes for the TEXTURECUBE case, fields only the D3D10_1 arm has, so
# the rendered surface is what the ABI actually requires; the mirror's link is
# stale. Both arms' own pages were fetched directly to confirm this rather than
# guessing from the parent page alone.

BUFFER_SRV = Struct(
    "D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW",
    [
        Union(arms=[Field("FirstElement", "UINT", *UINT)],
              published=[Field("FirstElement", "UINT", *UINT),
                         Field("ElementOffset", "UINT", *UINT)]),
        Union(arms=[Field("NumElements", "UINT", *UINT)],
              published=[Field("NumElements", "UINT", *UINT),
                         Field("ElementWidth", "UINT", *UINT)]),
    ],
)

TEX1D_SRV = Struct(
    "D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW",
    [
        Field("MostDetailedMip", "UINT", *UINT),
        Field("FirstArraySlice", "UINT", *UINT),
        Field("MipLevels", "UINT", *UINT),
        Field("ArraySize", "UINT", *UINT),
    ],
)

TEX2D_SRV = Struct(
    "D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW",
    [
        Field("MostDetailedMip", "UINT", *UINT),
        Field("FirstArraySlice", "UINT", *UINT),
        Field("MipLevels", "UINT", *UINT),
        Field("ArraySize", "UINT", *UINT),
        Field("PlaneSlice", "UINT", *UINT),
        Field("PlaneIndex", "UINT", *UINT),
    ],
)

TEX3D_SRV = Struct(
    "D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW",
    [
        Field("MostDetailedMip", "UINT", *UINT),
        Field("MipLevels", "UINT", *UINT),
    ],
)

TEXCUBE_SRV = Struct(
    "D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW",
    [
        Field("MostDetailedMip", "UINT", *UINT),
        Field("MipLevels", "UINT", *UINT),
        Field("First2DArrayFace", "UINT", *UINT),
        Field("NumCubes", "UINT", *UINT),
    ],
)

BUFFEREX_SRV = Struct(
    "D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW",
    [
        Union(arms=[Field("FirstElement", "UINT", *UINT)],
              published=[Field("FirstElement", "UINT", *UINT),
                         Field("ElementOffset", "UINT", *UINT)]),
        Union(arms=[Field("NumElements", "UINT", *UINT)],
              published=[Field("NumElements", "UINT", *UINT),
                         Field("ElementWidth", "UINT", *UINT)]),
        Field("Flags", "UINT", *UINT),
    ],
)

_SRV_ARM_TYPES = [
    ("Buffer", BUFFER_SRV),
    ("Tex1D", TEX1D_SRV),
    ("Tex2D", TEX2D_SRV),
    ("Tex3D", TEX3D_SRV),
    ("TexCube", TEXCUBE_SRV),
    ("BufferEx", BUFFEREX_SRV),
]

_SRV_ARM_FIELDS = [
    Field(name, arm.name, arm.walk()[1], arm.walk()[2])
    for name, arm in _SRV_ARM_TYPES
]

CREATESHADERRESOURCEVIEW = Struct(
    "D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW",
    [
        Field("hDrvResource", "D3D10DDI_HRESOURCE"),
        Field("Format", "DXGI_FORMAT", *UINT),
        Field("ResourceDimension", "D3D10DDIRESOURCE_TYPE", *UINT),
        Union(arms=_SRV_ARM_FIELDS, published=_SRV_ARM_FIELDS),
    ],
)

BUFFER_RTV = Struct(
    "D3D10DDIARG_BUFFER_RENDERTARGETVIEW",
    [
        Union(arms=[Field("FirstElement", "UINT", *UINT)],
              published=[Field("FirstElement", "UINT", *UINT),
                         Field("ElementOffset", "UINT", *UINT)]),
        Union(arms=[Field("NumElements", "UINT", *UINT)],
              published=[Field("NumElements", "UINT", *UINT),
                         Field("ElementWidth", "UINT", *UINT)]),
    ],
)

TEX1D_RTV = Struct(
    "D3D10DDIARG_TEX1D_RENDERTARGETVIEW",
    [
        Field("MipSlice", "UINT", *UINT),
        Field("FirstArraySlice", "UINT", *UINT),
        Field("ArraySize", "UINT", *UINT),
    ],
)

TEX2D_RTV = Struct(
    "D3D10DDIARG_TEX2D_RENDERTARGETVIEW",
    [
        Field("MipSlice", "UINT", *UINT),
        Field("FirstArraySlice", "UINT", *UINT),
        Field("ArraySize", "UINT", *UINT),
    ],
)

TEX3D_RTV = Struct(
    "D3D10DDIARG_TEX3D_RENDERTARGETVIEW",
    [
        Field("MipSlice", "UINT", *UINT),
        Field("FirstW", "UINT", *UINT),
        Field("WSize", "UINT", *UINT),
    ],
)

TEXCUBE_RTV = Struct(
    "D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW",
    [
        Field("MipSlice", "UINT", *UINT),
        Field("FirstArraySlice", "UINT", *UINT),
        Field("ArraySize", "UINT", *UINT),
    ],
)

_RTV_ARM_TYPES = [
    ("Buffer", BUFFER_RTV),
    ("Tex1D", TEX1D_RTV),
    ("Tex2D", TEX2D_RTV),
    ("Tex3D", TEX3D_RTV),
    ("TexCube", TEXCUBE_RTV),
]

_RTV_ARM_FIELDS = [
    Field(name, arm.name, arm.walk()[1], arm.walk()[2])
    for name, arm in _RTV_ARM_TYPES
]

CREATERENDERTARGETVIEW = Struct(
    "D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW",
    [
        Field("hDrvResource", "D3D10DDI_HRESOURCE"),
        Field("Format", "DXGI_FORMAT", *UINT),
        Field("ResourceDimension", "D3D10DDIRESOURCE_TYPE", *UINT),
        Union(arms=_RTV_ARM_FIELDS, published=_RTV_ARM_FIELDS),
    ],
)


# Group: adapter function tables and OpenAdapter arguments
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_openadapter
# Retrieved: 2026-09-06

ADAPTERFUNCS = Struct(
    "D3D10DDI_ADAPTERFUNCS",
    [
        Field("pfnCalcPrivateDeviceSize", "PFND3D10DDI_CALCPRIVATEDEVICESIZE"),
        Field("pfnCreateDevice", "PFND3D10DDI_CREATEDEVICE"),
        Field("pfnCloseAdapter", "PFND3D10DDI_CLOSEADAPTER"),
    ],
)

ADAPTERFUNCS_2 = Struct(
    "D3D10_2DDI_ADAPTERFUNCS",
    ADAPTERFUNCS.members
    + [
        Field("pfnGetSupportedVersions", "PFND3D10_2DDI_GETSUPPORTEDVERSIONS"),
        Field("pfnGetCaps", "PFND3D10_2DDI_GETCAPS"),
    ],
)

# Both arms are declared here, unlike the create-device unions below:
# OpenAdapter10 fills the first and OpenAdapter10_2 the second, and the host
# chooses which entry point it calls, so both are live.
OPENADAPTER = Struct(
    "D3D10DDIARG_OPENADAPTER",
    [
        Field("hRTAdapter", "D3D10DDI_HRTADAPTER"),
        Field("hAdapter", "D3D10DDI_HADAPTER"),
        Field("Interface", "UINT", *UINT),
        Field("Version", "UINT", *UINT),
        Field("pAdapterCallbacks", "const D3DDDI_ADAPTERCALLBACKS *"),
        Union(
            arms=[
                Field("pAdapterFuncs", "D3D10DDI_ADAPTERFUNCS *"),
                Field("pAdapterFuncs_2", "D3D10_2DDI_ADAPTERFUNCS *"),
            ],
            published=[
                Field("pAdapterFuncs", "D3D10DDI_ADAPTERFUNCS *"),
                Field("pAdapterFuncs_2", "D3D10_2DDI_ADAPTERFUNCS *"),
            ],
        ),
    ],
)

# Group: device creation
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_createdevice
#                https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_base_args
# Retrieved: 2026-09-07
#
# The driver reads DXGIBaseDDI.pDXGIDDIBaseFunctions6_1 in DeviceBase's
# constructor, GetDeviceFuncsFromCreateArgs returns pWDDM2_6DeviceFuncs, and
# the constructor reads pWDDM2_6UMCallbacks.  Those three are the declared
# arms; the rest are other DDI versions.

PUBLISHED_DXGI_ARMS = [
    Field("pDXGIDDIBaseFunctions6_1", "DXGI1_6_1_DDI_BASE_FUNCTIONS *"),
    Field("pDXGIDDIBaseFunctions6", "DXGI1_5_DDI_BASE_FUNCTIONS *"),
    Field("pDXGIDDIBaseFunctions5", "DXGI1_4_DDI_BASE_FUNCTIONS *"),
    Field("pDXGIDDIBaseFunctions4", "DXGI1_3_DDI_BASE_FUNCTIONS *"),
    Field("pDXGIDDIBaseFunctions3", "DXGI1_2_DDI_BASE_FUNCTIONS *"),
    Field("pDXGIDDIBaseFunctions2", "DXGI1_1_DDI_BASE_FUNCTIONS *"),
    Field("pDXGIDDIBaseFunctions", "DXGI_DDI_BASE_FUNCTIONS *"),
]

DXGI_BASE_ARGS = Struct(
    "DXGI_DDI_BASE_ARGS",
    [
        Field("pDXGIBaseCallbacks", "DXGI_DDI_BASE_CALLBACKS *"),
        Union(arms=PUBLISHED_DXGI_ARMS[:1], published=PUBLISHED_DXGI_ARMS),
    ],
)

PUBLISHED_DEVICEFUNC_ARMS = [
    Field("pDeviceFuncs", "D3D10DDI_DEVICEFUNCS *"),
    Field("p10_1DeviceFuncs", "D3D10_1DDI_DEVICEFUNCS *"),
    Field("p11DeviceFuncs", "D3D11DDI_DEVICEFUNCS *"),
    Field("p11_1DeviceFuncs", "D3D11_1DDI_DEVICEFUNCS *"),
    Field("pWDDM1_3DeviceFuncs", "D3DWDDM1_3DDI_DEVICEFUNCS *"),
    Field("pWDDM2_0DeviceFuncs", "D3DWDDM2_0DDI_DEVICEFUNCS *"),
    Field("pWDDM2_1DeviceFuncs", "D3DWDDM2_1DDI_DEVICEFUNCS *"),
    Field("pWDDM2_2DeviceFuncs", "D3DWDDM2_2DDI_DEVICEFUNCS *"),
    Field("pWDDM2_6DeviceFuncs", "D3DWDDM2_6DDI_DEVICEFUNCS *"),
]

PUBLISHED_CORELAYER_ARMS = [
    Field("pUMCallbacks", "const D3D10DDI_CORELAYER_DEVICECALLBACKS *"),
    Field("p11UMCallbacks", "const D3D11DDI_CORELAYER_DEVICECALLBACKS *"),
    Field(
        "pWDDM2_0UMCallbacks",
        "const D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS *",
    ),
    Field(
        "pWDDM2_2UMCallbacks",
        "const D3DWDDM2_2DDI_CORELAYER_DEVICECALLBACKS *",
    ),
    Field(
        "pWDDM2_6UMCallbacks",
        "const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *",
    ),
]

CREATEDEVICE = Struct(
    "D3D10DDIARG_CREATEDEVICE",
    [
        Field("hRTDevice", "D3D10DDI_HRTDEVICE"),
        Field("Interface", "UINT", *UINT),
        Field("Version", "UINT", *UINT),
        Field("pKTCallbacks", "const D3DDDI_DEVICECALLBACKS *"),
        Union(arms=PUBLISHED_DEVICEFUNC_ARMS[-1:], published=PUBLISHED_DEVICEFUNC_ARMS),
        Field("hDrvDevice", "D3D10DDI_HDEVICE"),
        Embedded("DXGIBaseDDI", DXGI_BASE_ARGS),
        Field("hRTCoreLayer", "D3D10DDI_HRTCORELAYER"),
        Union(arms=PUBLISHED_CORELAYER_ARMS[-1:], published=PUBLISHED_CORELAYER_ARMS),
        Field("Flags", "UINT", *UINT),
        Field("ppfnRetrieveSubObject", "PFND3D10DDI_RETRIEVESUBOBJECT *"),
    ],
)

# Group: core-layer device callbacks
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_6ddi_corelayer_devicecallbacks
# Retrieved: 2026-09-07
#
# 47 members in the order the rendered syntax block and the markdown mirror
# both give, every one a function pointer.  The member list is transcribed
# here from the specification rather than from the header, which is the whole
# point: a slot dropped or reordered in one artifact and not the other is
# exactly what this model exists to catch, and across 47 near-identical names
# it is a mistake a reader will not see.
#
# pfnDisableDeferredStagingResourceDestruction has no Cb suffix on either
# surface, and pfnShaderCacheAddRefCb and pfnShaderCacheReleaseCb share one
# type.  Both are as published.

CORELAYER_CALLBACK_SLOTS = [
    ("pfnSetErrorCb", "PFND3D10DDI_SETERROR_CB"),
    ("pfnStateVsConstBufCb", "PFND3D10DDI_STATE_VS_CONSTBUF_CB"),
    ("pfnStatePsSrvCb", "PFND3D10DDI_STATE_PS_SRV_CB"),
    ("pfnStatePsShaderCb", "PFND3D10DDI_STATE_PS_SHADER_CB"),
    ("pfnStatePsSamplerCb", "PFND3D10DDI_STATE_PS_SAMPLER_CB"),
    ("pfnStateVsShaderCb", "PFND3D10DDI_STATE_VS_SHADER_CB"),
    ("pfnStatePsConstBufCb", "PFND3D10DDI_STATE_PS_CONSTBUF_CB"),
    ("pfnStateIaInputLayoutCb", "PFND3D10DDI_STATE_IA_INPUTLAYOUT_CB"),
    ("pfnStateIaVertexBufCb", "PFND3D10DDI_STATE_IA_VERTEXBUF_CB"),
    ("pfnStateIaIndexBufCb", "PFND3D10DDI_STATE_IA_INDEXBUF_CB"),
    ("pfnStateGsConstBufCb", "PFND3D10DDI_STATE_GS_CONSTBUF_CB"),
    ("pfnStateGsShaderCb", "PFND3D10DDI_STATE_GS_SHADER_CB"),
    ("pfnStateIaPrimitiveTopologyCb",
     "PFND3D10DDI_STATE_IA_PRIMITIVE_TOPOLOGY_CB"),
    ("pfnStateVsSrvCb", "PFND3D10DDI_STATE_VS_SRV_CB"),
    ("pfnStateVsSamplerCb", "PFND3D10DDI_STATE_VS_SAMPLER_CB"),
    ("pfnStateGsSrvCb", "PFND3D10DDI_STATE_GS_SRV_CB"),
    ("pfnStateGsSamplerCb", "PFND3D10DDI_STATE_GS_SAMPLER_CB"),
    ("pfnStateOmRenderTargetsCb", "PFND3D10DDI_STATE_OM_RENDERTARGETS_CB"),
    ("pfnStateOmBlendStateCb", "PFND3D10DDI_STATE_OM_BLENDSTATE_CB"),
    ("pfnStateOmDepthStateCb", "PFND3D10DDI_STATE_OM_DEPTHSTATE_CB"),
    ("pfnStateRsRastStateCb", "PFND3D10DDI_STATE_RS_RASTSTATE_CB"),
    ("pfnStateSoTargetsCb", "PFND3D10DDI_STATE_SO_TARGETS_CB"),
    ("pfnStateRsViewportsCb", "PFND3D10DDI_STATE_RS_VIEWPORTS_CB"),
    ("pfnStateRsScissorCb", "PFND3D10DDI_STATE_RS_SCISSOR_CB"),
    ("pfnDisableDeferredStagingResourceDestruction",
     "PFND3D10DDI_DISABLE_DEFERRED_STAGING_RESOURCE_DESTRUCTION_CB"),
    ("pfnStateTextFilterSizeCb", "PFND3D10DDI_STATE_TEXTFILTERSIZE_CB"),
    ("pfnStateHsSrvCb", "PFND3D11DDI_STATE_HS_SRV_CB"),
    ("pfnStateHsShaderCb", "PFND3D11DDI_STATE_HS_SHADER_CB"),
    ("pfnStateHsSamplerCb", "PFND3D11DDI_STATE_HS_SAMPLER_CB"),
    ("pfnStateHsConstBufCb", "PFND3D11DDI_STATE_HS_CONSTBUF_CB"),
    ("pfnStateDsSrvCb", "PFND3D11DDI_STATE_DS_SRV_CB"),
    ("pfnStateDsShaderCb", "PFND3D11DDI_STATE_DS_SHADER_CB"),
    ("pfnStateDsSamplerCb", "PFND3D11DDI_STATE_DS_SAMPLER_CB"),
    ("pfnStateDsConstBufCb", "PFND3D11DDI_STATE_DS_CONSTBUF_CB"),
    ("pfnPerformAmortizedProcessingCb",
     "PFND3D11DDI_PERFORM_AMORTIZED_PROCESSING_CB"),
    ("pfnStateCsSrvCb", "PFND3D11DDI_STATE_CS_SRV_CB"),
    ("pfnStateCsUavCb", "PFND3D11DDI_STATE_CS_UAV_CB"),
    ("pfnStateCsShaderCb", "PFND3D11DDI_STATE_CS_SHADER_CB"),
    ("pfnStateCsSamplerCb", "PFND3D11DDI_STATE_CS_SAMPLER_CB"),
    ("pfnStateCsConstBufCb", "PFND3D11DDI_STATE_CS_CONSTBUF_CB"),
    ("pfnCreateContextCb", "PFND3DWDDM2_0DDI_CREATECONTEXT_CB"),
    ("pfnCreateContextVirtualCb", "PFND3DWDDM2_0DDI_CREATECONTEXTVIRTUAL_CB"),
    ("pfnShaderCacheGetValueCb", "PFND3DWDDM2_2DDI_SHADERCACHE_GET_VALUE_CB"),
    ("pfnShaderCacheStoreValueCb",
     "PFND3DWDDM2_2DDI_SHADERCACHE_STORE_VALUE_CB"),
    ("pfnShaderCacheAddRefCb",
     "PFND3DWDDM2_2DDI_SHADERCACHE_ADDREF_RELEASE_CB"),
    ("pfnShaderCacheReleaseCb",
     "PFND3DWDDM2_2DDI_SHADERCACHE_ADDREF_RELEASE_CB"),
    ("pfnQueryScanoutCapsCb", "PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS_CB"),
]

CORELAYER_CALLBACKS = Struct(
    "D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS",
    [Field(name, type_name) for name, type_name in CORELAYER_CALLBACK_SLOTS],
)

# The published member count, held separately from the list above so that
# losing a line from it is a failure rather than a smaller table that agrees
# with itself.
if len(CORELAYER_CALLBACK_SLOTS) != 47:
    raise SystemExit(
        "the core-layer callback table is published with 47 members, "
        f"the model lists {len(CORELAYER_CALLBACK_SLOTS)}"
    )

# Group: kernel device callbacks
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddi_devicecallbacks
#                https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_escape
#                https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_synctoken
# Retrieved: 2026-09-07
#
# The rendered syntax block and the markdown mirror disagree at the tail: 66
# members against 65, the odd one being pfnCreateNativeFenceCb, a WDDM 3.1
# addition the mirror has not caught up with.  The superset is modelled, for
# the reason the header records.  Everything the pinned driver reads is at
# index 9, 53 and 54, identical under both surfaces, so the disagreement
# cannot move a live offset.
#
# Transcribed from the specification rather than from the header, as the
# core-layer list is: across 66 near-identical names, a slot dropped in one
# artifact and not the other is not a mistake a reader will see.

KERNEL_CALLBACK_SLOTS = [
    ("pfnAllocateCb", "PFND3DDDI_ALLOCATECB"),
    ("pfnDeallocateCb", "PFND3DDDI_DEALLOCATECB"),
    ("pfnSetPriorityCb", "PFND3DDDI_SETPRIORITYCB"),
    ("pfnQueryResidencyCb", "PFND3DDDI_QUERYRESIDENCYCB"),
    ("pfnSetDisplayModeCb", "PFND3DDDI_SETDISPLAYMODECB"),
    ("pfnPresentCb", "PFND3DDDI_PRESENTCB"),
    ("pfnRenderCb", "PFND3DDDI_RENDERCB"),
    ("pfnLockCb", "PFND3DDDI_LOCKCB"),
    ("pfnUnlockCb", "PFND3DDDI_UNLOCKCB"),
    ("pfnEscapeCb", "PFND3DDDI_ESCAPECB"),
    ("pfnCreateOverlayCb", "PFND3DDDI_CREATEOVERLAYCB"),
    ("pfnUpdateOverlayCb", "PFND3DDDI_UPDATEOVERLAYCB"),
    ("pfnFlipOverlayCb", "PFND3DDDI_FLIPOVERLAYCB"),
    ("pfnDestroyOverlayCb", "PFND3DDDI_DESTROYOVERLAYCB"),
    ("pfnCreateContextCb", "PFND3DDDI_CREATECONTEXTCB"),
    ("pfnDestroyContextCb", "PFND3DDDI_DESTROYCONTEXTCB"),
    ("pfnCreateSynchronizationObjectCb",
     "PFND3DDDI_CREATESYNCHRONIZATIONOBJECTCB"),
    ("pfnDestroySynchronizationObjectCb",
     "PFND3DDDI_DESTROYSYNCHRONIZATIONOBJECTCB"),
    ("pfnWaitForSynchronizationObjectCb",
     "PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTCB"),
    ("pfnSignalSynchronizationObjectCb",
     "PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTCB"),
    ("pfnSetAsyncCallbacksCb", "PFND3DDDI_SETASYNCCALLBACKSCB"),
    ("pfnSetDisplayPrivateDriverFormatCb",
     "PFND3DDDI_SETDISPLAYPRIVATEDRIVERFORMATCB"),
    ("pfnOfferAllocationsCb", "PFND3DDDI_OFFERALLOCATIONSCB"),
    ("pfnReclaimAllocationsCb", "PFND3DDDI_RECLAIMALLOCATIONSCB"),
    ("pfnCreateSynchronizationObject2Cb",
     "PFND3DDDI_CREATESYNCHRONIZATIONOBJECT2CB"),
    ("pfnWaitForSynchronizationObject2Cb",
     "PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECT2CB"),
    ("pfnSignalSynchronizationObject2Cb",
     "PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECT2CB"),
    ("pfnPresentMultiPlaneOverlayCb", "PFND3DDDI_PRESENTMULTIPLANEOVERLAYCB"),
    ("pfnLogUMDMarkerCb", "PFND3DDDI_LOGUMDMARKERCB"),
    ("pfnMakeResidentCb", "PFND3DDDI_MAKERESIDENTCB"),
    ("pfnEvictCb", "PFND3DDDI_EVICTCB"),
    ("pfnWaitForSynchronizationObjectFromCpuCb",
     "PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPUCB"),
    ("pfnSignalSynchronizationObjectFromCpuCb",
     "PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMCPUCB"),
    ("pfnWaitForSynchronizationObjectFromGpuCb",
     "PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMGPUCB"),
    ("pfnSignalSynchronizationObjectFromGpuCb",
     "PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMGPUCB"),
    ("pfnCreatePagingQueueCb", "PFND3DDDI_CREATEPAGINGQUEUECB"),
    ("pfnDestroyPagingQueueCb", "PFND3DDDI_DESTROYPAGINGQUEUECB"),
    ("pfnLock2Cb", "PFND3DDDI_LOCK2CB"),
    ("pfnUnlock2Cb", "PFND3DDDI_UNLOCK2CB"),
    ("pfnInvalidateCacheCb", "PFND3DDDI_INVALIDATECACHECB"),
    ("pfnReserveGpuVirtualAddressCb",
     "PFND3DDDI_RESERVEGPUVIRTUALADDRESSCB"),
    ("pfnMapGpuVirtualAddressCb", "PFND3DDDI_MAPGPUVIRTUALADDRESSCB"),
    ("pfnFreeGpuVirtualAddressCb", "PFND3DDDI_FREEGPUVIRTUALADDRESSCB"),
    ("pfnUpdateGpuVirtualAddressCb", "PFND3DDDI_UPDATEGPUVIRTUALADDRESSCB"),
    ("pfnCreateContextVirtualCb", "PFND3DDDI_CREATECONTEXTVIRTUALCB"),
    ("pfnSubmitCommandCb", "PFND3DDDI_SUBMITCOMMANDCB"),
    ("pfnDeallocate2Cb", "PFND3DDDI_DEALLOCATE2CB"),
    ("pfnSignalSynchronizationObjectFromGpu2Cb",
     "PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2CB"),
    ("pfnReclaimAllocations2Cb", "PFND3DDDI_RECLAIMALLOCATIONS2CB"),
    ("pfnGetResourcePresentPrivateDriverDataCb",
     "PFND3DDDI_GETRESOURCEPRESENTPRIVATEDRIVERDATACB"),
    ("pfnUpdateAllocationPropertyCb",
     "PFND3DDDI_UPDATEALLOCATIONPROPERTYCB"),
    ("pfnOfferAllocations2Cb", "PFND3DDDI_OFFERALLOCATIONS2CB"),
    ("pfnReclaimAllocations3Cb", "PFND3DDDI_RECLAIMALLOCATIONS3CB"),
    ("pfnAcquireResourceCb", "PFND3DDDI_SYNCTOKENCB"),
    ("pfnReleaseResourceCb", "PFND3DDDI_SYNCTOKENCB"),
    ("pfnCreateHwContextCb", "PFND3DDDI_CREATEHWCONTEXTCB"),
    ("pfnDestroyHwContextCb", "PFND3DDDI_DESTROYHWCONTEXTCB"),
    ("pfnCreateHwQueueCb", "PFND3DDDI_CREATEHWQUEUECB"),
    ("pfnDestroyHwQueueCb", "PFND3DDDI_DESTROYHWQUEUECB"),
    ("pfnSubmitCommandToHwQueueCb", "PFND3DDDI_SUBMITCOMMANDTOHWQUEUECB"),
    ("pfnSubmitWaitForSyncObjectsToHwQueueCb",
     "PFND3DDDI_SUBMITWAITFORSYNCOBJECTSTOHWQUEUECB"),
    ("pfnSubmitSignalSyncObjectsToHwQueueCb",
     "PFND3DDDI_SUBMITSIGNALSYNCOBJECTSTOHWQUEUECB"),
    ("pfnSubmitPresentBltToHwQueueCb",
     "PFND3DDDI_SUBMITPRESENTBLTTOHWQUEUECB"),
    ("pfnSubmitPresentToHwQueueCb", "PFND3DDDI_SUBMITPRESENTTOHWQUEUECB"),
    ("pfnSubmitHistorySequenceCb", "PFND3DDDI_SUBMITHISTORYSEQUENCECB"),
    ("pfnCreateNativeFenceCb", "PFND3DDDI_CREATENATIVEFENCECB"),
]

KERNEL_CALLBACKS = Struct(
    "D3DDDI_DEVICECALLBACKS",
    [Field(name, type_name) for name, type_name in KERNEL_CALLBACK_SLOTS],
)

# The slots the pinned driver actually reads, by index.  Held here so that the
# claim the header makes about them is checked rather than asserted in prose.
KERNEL_CALLBACKS_INVOKED = {
    9: "pfnEscapeCb",
    53: "pfnAcquireResourceCb",
    54: "pfnReleaseResourceCb",
}

# The escape flags are a 32-bit bitfield union.  This model has no bitfield
# vocabulary and does not need one: what it owes the enclosing structure is
# four bytes at four-byte alignment, and the bit positions are pinned at run
# time by tests/d3d11ddilayout.c instead.
ESCAPE = Struct(
    "D3DDDICB_ESCAPE",
    [
        Field("hDevice", "HANDLE"),
        Field("Flags", "WINE_D3D11DDI_ESCAPEFLAGS", *UINT),
        Field("pPrivateDriverData", "void *"),
        Field("PrivateDriverDataSize", "UINT", *UINT),
        Field("hContext", "HANDLE"),
    ],
)

SYNCTOKEN = Struct(
    "D3DDDICB_SYNCTOKEN",
    [
        Field("hSyncToken", "HANDLE"),
        Field("BroadcastContextCount", "UINT", *UINT),
        Field("BroadcastContextArray", "const HANDLE *"),
    ],
)

if len(KERNEL_CALLBACK_SLOTS) != 66:
    raise SystemExit(
        "the kernel callback table is published with 66 members on the "
        f"rendered surface, the model lists {len(KERNEL_CALLBACK_SLOTS)}"
    )

for _index, _name in KERNEL_CALLBACKS_INVOKED.items():
    if KERNEL_CALLBACK_SLOTS[_index][0] != _name:
        raise SystemExit(
            f"the pinned driver reads {_name} at index {_index}, but the "
            f"model puts {KERNEL_CALLBACK_SLOTS[_index][0]} there"
        )

# Group: WDDM 2.6 device function table
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_6ddi_devicefuncs
# Source mirror: https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ns-d3d10umddi-d3dwddm2_6ddi_devicefuncs.md
# Retrieved: 2026-09-08
#
# The driver fills this table and the host controls which slots it invokes.
# Preserve every published PFN name for incremental signature promotion, but
# model every slot as one pointer until its signature is authored.

DEVICEFUNC_SLOTS = [
    ("pfnDefaultConstantBufferUpdateSubresourceUP", "PFND3D11_1DDI_RESOURCEUPDATESUBRESOURCEUP"),
    ("pfnVsSetConstantBuffers", "PFND3D11_1DDI_SETCONSTANTBUFFERS"),
    ("pfnPsSetShaderResources", "PFND3D10DDI_SETSHADERRESOURCES"),
    ("pfnPsSetShader", "PFND3D10DDI_SETSHADER"),
    ("pfnPsSetSamplers", "PFND3D10DDI_SETSAMPLERS"),
    ("pfnVsSetShader", "PFND3D10DDI_SETSHADER"),
    ("pfnDrawIndexed", "PFND3D10DDI_DRAWINDEXED"),
    ("pfnDraw", "PFND3D10DDI_DRAW"),
    ("pfnDynamicIABufferMapNoOverwrite", "PFND3D10DDI_RESOURCEMAP"),
    ("pfnDynamicIABufferUnmap", "PFND3D10DDI_RESOURCEUNMAP"),
    ("pfnDynamicConstantBufferMapDiscard", "PFND3D10DDI_RESOURCEMAP"),
    ("pfnDynamicIABufferMapDiscard", "PFND3D10DDI_RESOURCEMAP"),
    ("pfnDynamicConstantBufferUnmap", "PFND3D10DDI_RESOURCEUNMAP"),
    ("pfnPsSetConstantBuffers", "PFND3D11_1DDI_SETCONSTANTBUFFERS"),
    ("pfnIaSetInputLayout", "PFND3D10DDI_SETINPUTLAYOUT"),
    ("pfnIaSetVertexBuffers", "PFND3D10DDI_IA_SETVERTEXBUFFERS"),
    ("pfnIaSetIndexBuffer", "PFND3D10DDI_IA_SETINDEXBUFFER"),
    ("pfnDrawIndexedInstanced", "PFND3D10DDI_DRAWINDEXEDINSTANCED"),
    ("pfnDrawInstanced", "PFND3D10DDI_DRAWINSTANCED"),
    ("pfnDynamicResourceMapDiscard", "PFND3D10DDI_RESOURCEMAP"),
    ("pfnDynamicResourceUnmap", "PFND3D10DDI_RESOURCEUNMAP"),
    ("pfnGsSetConstantBuffers", "PFND3D11_1DDI_SETCONSTANTBUFFERS"),
    ("pfnGsSetShader", "PFND3D10DDI_SETSHADER"),
    ("pfnIaSetTopology", "PFND3D10DDI_IA_SETTOPOLOGY"),
    ("pfnStagingResourceMap", "PFND3D10DDI_RESOURCEMAP"),
    ("pfnStagingResourceUnmap", "PFND3D10DDI_RESOURCEUNMAP"),
    ("pfnVsSetShaderResources", "PFND3D10DDI_SETSHADERRESOURCES"),
    ("pfnVsSetSamplers", "PFND3D10DDI_SETSAMPLERS"),
    ("pfnGsSetShaderResources", "PFND3D10DDI_SETSHADERRESOURCES"),
    ("pfnGsSetSamplers", "PFND3D10DDI_SETSAMPLERS"),
    ("pfnSetRenderTargets", "PFND3D11DDI_SETRENDERTARGETS"),
    ("pfnShaderResourceViewReadAfterWriteHazard", "PFND3D10DDI_SHADERRESOURCEVIEWREADAFTERWRITEHAZARD"),
    ("pfnResourceReadAfterWriteHazard", "PFND3D10DDI_RESOURCEREADAFTERWRITEHAZARD"),
    ("pfnSetBlendState", "PFND3D10DDI_SETBLENDSTATE"),
    ("pfnSetDepthStencilState", "PFND3D10DDI_SETDEPTHSTENCILSTATE"),
    ("pfnSetRasterizerState", "PFND3D10DDI_SETRASTERIZERSTATE"),
    ("pfnQueryEnd", "PFND3D10DDI_QUERYEND"),
    ("pfnQueryBegin", "PFND3D10DDI_QUERYBEGIN"),
    ("pfnResourceCopyRegion", "PFND3D11_1DDI_RESOURCECOPYREGION"),
    ("pfnResourceUpdateSubresourceUP", "PFND3D11_1DDI_RESOURCEUPDATESUBRESOURCEUP"),
    ("pfnSoSetTargets", "PFND3D10DDI_SO_SETTARGETS"),
    ("pfnDrawAuto", "PFND3D10DDI_DRAWAUTO"),
    ("pfnSetViewports", "PFND3D10DDI_SETVIEWPORTS"),
    ("pfnSetScissorRects", "PFND3D10DDI_SETSCISSORRECTS"),
    ("pfnClearRenderTargetView", "PFND3D10DDI_CLEARRENDERTARGETVIEW"),
    ("pfnClearDepthStencilView", "PFND3D10DDI_CLEARDEPTHSTENCILVIEW"),
    ("pfnSetPredication", "PFND3D10DDI_SETPREDICATION"),
    ("pfnQueryGetData", "PFND3D10DDI_QUERYGETDATA"),
    ("pfnFlush", "PFND3DWDDM2_0DDI_FLUSH"),
    ("pfnGenMips", "PFND3D10DDI_GENMIPS"),
    ("pfnResourceCopy", "PFND3D10DDI_RESOURCECOPY"),
    ("pfnResourceResolveSubresource", "PFND3D10DDI_RESOURCERESOLVESUBRESOURCE"),
    ("pfnResourceMap", "PFND3D10DDI_RESOURCEMAP"),
    ("pfnResourceUnmap", "PFND3D10DDI_RESOURCEUNMAP"),
    ("pfnResourceIsStagingBusy", "PFND3D10DDI_RESOURCEISSTAGINGBUSY"),
    ("pfnRelocateDeviceFuncs", "PFND3DWDDM2_6DDI_RELOCATEDEVICEFUNCS"),
    ("pfnCalcPrivateResourceSize", "PFND3D11DDI_CALCPRIVATERESOURCESIZE"),
    ("pfnCalcPrivateOpenedResourceSize", "PFND3D10DDI_CALCPRIVATEOPENEDRESOURCESIZE"),
    ("pfnCreateResource", "PFND3D11DDI_CREATERESOURCE"),
    ("pfnOpenResource", "PFND3D10DDI_OPENRESOURCE"),
    ("pfnDestroyResource", "PFND3D10DDI_DESTROYRESOURCE"),
    ("pfnCalcPrivateShaderResourceViewSize", "PFND3DWDDM2_0DDI_CALCPRIVATESHADERRESOURCEVIEWSIZE"),
    ("pfnCreateShaderResourceView", "PFND3DWDDM2_0DDI_CREATESHADERRESOURCEVIEW"),
    ("pfnDestroyShaderResourceView", "PFND3D10DDI_DESTROYSHADERRESOURCEVIEW"),
    ("pfnCalcPrivateRenderTargetViewSize", "PFND3DWDDM2_0DDI_CALCPRIVATERENDERTARGETVIEWSIZE"),
    ("pfnCreateRenderTargetView", "PFND3DWDDM2_0DDI_CREATERENDERTARGETVIEW"),
    ("pfnDestroyRenderTargetView", "PFND3D10DDI_DESTROYRENDERTARGETVIEW"),
    ("pfnCalcPrivateDepthStencilViewSize", "PFND3D11DDI_CALCPRIVATEDEPTHSTENCILVIEWSIZE"),
    ("pfnCreateDepthStencilView", "PFND3D11DDI_CREATEDEPTHSTENCILVIEW"),
    ("pfnDestroyDepthStencilView", "PFND3D10DDI_DESTROYDEPTHSTENCILVIEW"),
    ("pfnCalcPrivateElementLayoutSize", "PFND3D10DDI_CALCPRIVATEELEMENTLAYOUTSIZE"),
    ("pfnCreateElementLayout", "PFND3D10DDI_CREATEELEMENTLAYOUT"),
    ("pfnDestroyElementLayout", "PFND3D10DDI_DESTROYELEMENTLAYOUT"),
    ("pfnCalcPrivateBlendStateSize", "PFND3D11_1DDI_CALCPRIVATEBLENDSTATESIZE"),
    ("pfnCreateBlendState", "PFND3D11_1DDI_CREATEBLENDSTATE"),
    ("pfnDestroyBlendState", "PFND3D10DDI_DESTROYBLENDSTATE"),
    ("pfnCalcPrivateDepthStencilStateSize", "PFND3D10DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE"),
    ("pfnCreateDepthStencilState", "PFND3D10DDI_CREATEDEPTHSTENCILSTATE"),
    ("pfnDestroyDepthStencilState", "PFND3D10DDI_DESTROYDEPTHSTENCILSTATE"),
    ("pfnCalcPrivateRasterizerStateSize", "PFND3DWDDM2_0DDI_CALCPRIVATERASTERIZERSTATESIZE"),
    ("pfnCreateRasterizerState", "PFND3DWDDM2_0DDI_CREATERASTERIZERSTATE"),
    ("pfnDestroyRasterizerState", "PFND3D10DDI_DESTROYRASTERIZERSTATE"),
    ("pfnCalcPrivateShaderSize", "PFND3D11_1DDI_CALCPRIVATESHADERSIZE"),
    ("pfnCreateVertexShader", "PFND3D11_1DDI_CREATEVERTEXSHADER"),
    ("pfnCreateGeometryShader", "PFND3D11_1DDI_CREATEGEOMETRYSHADER"),
    ("pfnCreatePixelShader", "PFND3D11_1DDI_CREATEPIXELSHADER"),
    ("pfnCalcPrivateGeometryShaderWithStreamOutput", "PFND3D11_1DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT"),
    ("pfnCreateGeometryShaderWithStreamOutput", "PFND3D11_1DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT"),
    ("pfnDestroyShader", "PFND3D10DDI_DESTROYSHADER"),
    ("pfnCalcPrivateSamplerSize", "PFND3D10DDI_CALCPRIVATESAMPLERSIZE"),
    ("pfnCreateSampler", "PFND3D10DDI_CREATESAMPLER"),
    ("pfnDestroySampler", "PFND3D10DDI_DESTROYSAMPLER"),
    ("pfnCalcPrivateQuerySize", "PFND3DWDDM2_0DDI_CALCPRIVATEQUERYSIZE"),
    ("pfnCreateQuery", "PFND3DWDDM2_0DDI_CREATEQUERY"),
    ("pfnDestroyQuery", "PFND3D10DDI_DESTROYQUERY"),
    ("pfnCheckFormatSupport", "PFND3D10DDI_CHECKFORMATSUPPORT"),
    ("pfnCheckMultisampleQualityLevels", "PFND3DWDDM1_3DDI_CHECKMULTISAMPLEQUALITYLEVELS"),
    ("pfnCheckCounterInfo", "PFND3D10DDI_CHECKCOUNTERINFO"),
    ("pfnCheckCounter", "PFND3D10DDI_CHECKCOUNTER"),
    ("pfnDestroyDevice", "PFND3D10DDI_DESTROYDEVICE"),
    ("pfnSetTextFilterSize", "PFND3D10DDI_SETTEXTFILTERSIZE"),
    ("pfnResourceConvert", "PFND3D10DDI_RESOURCECOPY"),
    ("pfnResourceConvertRegion", "PFND3D11_1DDI_RESOURCECOPYREGION"),
    ("pfnResetPrimitiveID", "PFND3D10DDI_RESETPRIMITIVEID"),
    ("pfnSetVertexPipelineOutput", "PFND3D10DDI_SETVERTEXPIPELINEOUTPUT"),
    ("pfnDrawIndexedInstancedIndirect", "PFND3D11DDI_DRAWINDEXEDINSTANCEDINDIRECT"),
    ("pfnDrawInstancedIndirect", "PFND3D11DDI_DRAWINSTANCEDINDIRECT"),
    ("pfnCommandListExecute", "PFND3D11DDI_COMMANDLISTEXECUTE"),
    ("pfnHsSetShaderResources", "PFND3D10DDI_SETSHADERRESOURCES"),
    ("pfnHsSetShader", "PFND3D10DDI_SETSHADER"),
    ("pfnHsSetSamplers", "PFND3D10DDI_SETSAMPLERS"),
    ("pfnHsSetConstantBuffers", "PFND3D11_1DDI_SETCONSTANTBUFFERS"),
    ("pfnDsSetShaderResources", "PFND3D10DDI_SETSHADERRESOURCES"),
    ("pfnDsSetShader", "PFND3D10DDI_SETSHADER"),
    ("pfnDsSetSamplers", "PFND3D10DDI_SETSAMPLERS"),
    ("pfnDsSetConstantBuffers", "PFND3D11_1DDI_SETCONSTANTBUFFERS"),
    ("pfnCreateHullShader", "PFND3D11_1DDI_CREATEHULLSHADER"),
    ("pfnCreateDomainShader", "PFND3D11_1DDI_CREATEDOMAINSHADER"),
    ("pfnCheckDeferredContextHandleSizes", "PFND3D11DDI_CHECKDEFERREDCONTEXTHANDLESIZES"),
    ("pfnCalcDeferredContextHandleSize", "PFND3D11DDI_CALCDEFERREDCONTEXTHANDLESIZE"),
    ("pfnCalcPrivateDeferredContextSize", "PFND3D11DDI_CALCPRIVATEDEFERREDCONTEXTSIZE"),
    ("pfnCreateDeferredContext", "PFND3D11DDI_CREATEDEFERREDCONTEXT"),
    ("pfnAbandonCommandList", "PFND3D11DDI_ABANDONCOMMANDLIST"),
    ("pfnCalcPrivateCommandListSize", "PFND3D11DDI_CALCPRIVATECOMMANDLISTSIZE"),
    ("pfnCreateCommandList", "PFND3D11DDI_CREATECOMMANDLIST"),
    ("pfnDestroyCommandList", "PFND3D11DDI_DESTROYCOMMANDLIST"),
    ("pfnCalcPrivateTessellationShaderSize", "PFND3D11_1DDI_CALCPRIVATETESSELLATIONSHADERSIZE"),
    ("pfnPsSetShaderWithIfaces", "PFND3D11DDI_SETSHADER_WITH_IFACES"),
    ("pfnVsSetShaderWithIfaces", "PFND3D11DDI_SETSHADER_WITH_IFACES"),
    ("pfnGsSetShaderWithIfaces", "PFND3D11DDI_SETSHADER_WITH_IFACES"),
    ("pfnHsSetShaderWithIfaces", "PFND3D11DDI_SETSHADER_WITH_IFACES"),
    ("pfnDsSetShaderWithIfaces", "PFND3D11DDI_SETSHADER_WITH_IFACES"),
    ("pfnCsSetShaderWithIfaces", "PFND3D11DDI_SETSHADER_WITH_IFACES"),
    ("pfnCreateComputeShader", "PFND3D11DDI_CREATECOMPUTESHADER"),
    ("pfnCsSetShader", "PFND3D10DDI_SETSHADER"),
    ("pfnCsSetShaderResources", "PFND3D10DDI_SETSHADERRESOURCES"),
    ("pfnCsSetSamplers", "PFND3D10DDI_SETSAMPLERS"),
    ("pfnCsSetConstantBuffers", "PFND3D11_1DDI_SETCONSTANTBUFFERS"),
    ("pfnCalcPrivateUnorderedAccessViewSize", "PFND3DWDDM2_0DDI_CALCPRIVATEUNORDEREDACCESSVIEWSIZE"),
    ("pfnCreateUnorderedAccessView", "PFND3DWDDM2_0DDI_CREATEUNORDEREDACCESSVIEW"),
    ("pfnDestroyUnorderedAccessView", "PFND3D11DDI_DESTROYUNORDEREDACCESSVIEW"),
    ("pfnClearUnorderedAccessViewUint", "PFND3D11DDI_CLEARUNORDEREDACCESSVIEWUINT"),
    ("pfnClearUnorderedAccessViewFloat", "PFND3D11DDI_CLEARUNORDEREDACCESSVIEWFLOAT"),
    ("pfnCsSetUnorderedAccessViews", "PFND3D11DDI_SETUNORDEREDACCESSVIEWS"),
    ("pfnDispatch", "PFND3D11DDI_DISPATCH"),
    ("pfnDispatchIndirect", "PFND3D11DDI_DISPATCHINDIRECT"),
    ("pfnSetResourceMinLOD", "PFND3D11DDI_SETRESOURCEMINLOD"),
    ("pfnCopyStructureCount", "PFND3D11DDI_COPYSTRUCTURECOUNT"),
    ("pfnRecycleCommandList", "PFND3D11DDI_RECYCLECOMMANDLIST"),
    ("pfnRecycleCreateCommandList", "PFND3D11DDI_RECYCLECREATECOMMANDLIST"),
    ("pfnRecycleCreateDeferredContext", "PFND3D11DDI_RECYCLECREATEDEFERREDCONTEXT"),
    ("pfnRecycleDestroyCommandList", "PFND3D11DDI_DESTROYCOMMANDLIST"),
    ("pfnDiscard", "PFND3D11_1DDI_DISCARD"),
    ("pfnAssignDebugBinary", "PFND3D11_1DDI_ASSIGNDEBUGBINARY"),
    ("pfnDynamicConstantBufferMapNoOverwrite", "PFND3D10DDI_RESOURCEMAP"),
    ("pfnCheckDirectFlipSupport", "PFND3D11_1DDI_CHECKDIRECTFLIPSUPPORT"),
    ("pfnClearView", "PFND3D11_1DDI_CLEARVIEW"),
    ("pfnUpdateTileMappings", "PFND3DWDDM1_3DDI_UPDATETILEMAPPINGS"),
    ("pfnCopyTileMappings", "PFND3DWDDM1_3DDI_COPYTILEMAPPINGS"),
    ("pfnCopyTiles", "PFND3DWDDM1_3DDI_COPYTILES"),
    ("pfnUpdateTiles", "PFND3DWDDM1_3DDI_UPDATETILES"),
    ("pfnTiledResourceBarrier", "PFND3DWDDM1_3DDI_TILEDRESOURCEBARRIER"),
    ("pfnGetMipPacking", "PFND3DWDDM1_3DDI_GETMIPPACKING"),
    ("pfnResizeTilePool", "PFND3DWDDM1_3DDI_RESIZETILEPOOL"),
    ("pfnSetMarker", "PFND3DWDDM1_3DDI_SETMARKER"),
    ("pfnSetMarkerMode", "PFND3DWDDM1_3DDI_SETMARKERMODE"),
    ("pfnSetHardwareProtection", "PFND3DWDDM2_0DDI_SETHARDWAREPROTECTION"),
    ("pfnGetResourceLayout", "PFND3DWDDM2_0DDI_GETRESOURCELAYOUT"),
    ("pfnRetrieveShaderComment", "PFND3DWDDM2_0DDI_RETRIEVE_SHADER_COMMENT"),
    ("pfnSetHardwareProtectionState", "PFND3DWDDM2_0DDI_SETHARDWAREPROTECTIONSTATE"),
    ("pfnAcquireResource", "PFND3DWDDM2_1DDI_SYNC_TOKEN"),
    ("pfnReleaseResource", "PFND3DWDDM2_1DDI_SYNC_TOKEN"),
    ("pfnCalcPrivateShaderCacheSessionSize", "PFND3DWDDM2_2DDI_CALCPRIVATE_SHADERCACHE_SESSION_SIZE"),
    ("pfnCreateShaderCacheSession", "PFND3DWDDM2_2DDI_CREATE_SHADERCACHE_SESSION"),
    ("pfnDestroyShaderCacheSession", "PFND3DWDDM2_2DDI_DESTROY_SHADERCACHE_SESSION"),
    ("pfnSetShaderCacheSession", "PFND3DWDDM2_2DDI_SET_SHADERCACHE_SESSION"),
    ("pfnQueryScanoutCaps", "PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS"),
    ("pfnPrepareScanoutTransformation", "PFND3DWDDM2_6DDI_PREPARE_SCANOUT_TRANSFORMATION"),
]

DEVICEFUNCS = Struct(
    "D3DWDDM2_6DDI_DEVICEFUNCS",
    [Field(name, type_name) for name, type_name in DEVICEFUNC_SLOTS],
)

# The PFN typedefs whose signature has been authored from its own reference
# page, so the header must declare them as function types rather than as
# aliases of the no-argument placeholder.
#
# Held here, apart from the slot list, because promotion is the one change to
# this table that no layout assertion can see: a promoted pointer and a
# placeholder pointer are both eight bytes at the same offset.  The set is
# what makes a promotion land completely -- a name in it that is still an
# alias fails, and a name absent from it that is no longer an alias fails too,
# so a slot cannot be promoted in the header and forgotten here, or regress to
# a placeholder without anyone noticing.
#
# The command-list family: every parameter is a handle, so they need the
# command-list handle group and no argument structure.  See the group note in
# the header for why the rest of the family cannot follow yet.
PROMOTED_SLOTS = {
    "PFND3D10DDI_RESOURCEMAP",
    "PFND3D10DDI_RESOURCEUNMAP",
    "PFND3D10DDI_RESOURCECOPY",
    "PFND3D11DDI_ABANDONCOMMANDLIST",
    "PFND3D11DDI_CALCPRIVATECOMMANDLISTSIZE",
    "PFND3D11DDI_COMMANDLISTEXECUTE",
    "PFND3D11DDI_CREATECOMMANDLIST",
    "PFND3D11DDI_DESTROYCOMMANDLIST",
    "PFND3D11DDI_RECYCLECOMMANDLIST",
    "PFND3D11DDI_RECYCLECREATECOMMANDLIST",
    "PFND3D11DDI_CHECKDEFERREDCONTEXTHANDLESIZES",
    "PFND3D11DDI_CALCDEFERREDCONTEXTHANDLESIZE",
    "PFND3D11DDI_CALCPRIVATEDEFERREDCONTEXTSIZE",
    "PFND3D11DDI_CREATEDEFERREDCONTEXT",
    "PFND3D11DDI_RECYCLECREATEDEFERREDCONTEXT",
    "PFND3D11DDI_CALCPRIVATERESOURCESIZE",
    "PFND3D10DDI_CALCPRIVATEOPENEDRESOURCESIZE",
    "PFND3D11DDI_CREATERESOURCE",
    "PFND3D10DDI_OPENRESOURCE",
    "PFND3D10DDI_DESTROYRESOURCE",
    "PFND3D11_1DDI_CALCPRIVATEBLENDSTATESIZE",
    "PFND3D11_1DDI_CREATEBLENDSTATE",
    "PFND3D10DDI_DESTROYBLENDSTATE",
    "PFND3D10DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE",
    "PFND3D10DDI_CREATEDEPTHSTENCILSTATE",
    "PFND3D10DDI_DESTROYDEPTHSTENCILSTATE",
    "PFND3DWDDM2_0DDI_CALCPRIVATERASTERIZERSTATESIZE",
    "PFND3DWDDM2_0DDI_CREATERASTERIZERSTATE",
    "PFND3D10DDI_DESTROYRASTERIZERSTATE",
    "PFND3D10DDI_CALCPRIVATESAMPLERSIZE",
    "PFND3D10DDI_CREATESAMPLER",
    "PFND3D10DDI_DESTROYSAMPLER",
    "PFND3DWDDM2_0DDI_CALCPRIVATESHADERRESOURCEVIEWSIZE",
    "PFND3DWDDM2_0DDI_CREATESHADERRESOURCEVIEW",
    "PFND3D10DDI_DESTROYSHADERRESOURCEVIEW",
    "PFND3DWDDM2_0DDI_CALCPRIVATERENDERTARGETVIEWSIZE",
    "PFND3DWDDM2_0DDI_CREATERENDERTARGETVIEW",
    "PFND3D10DDI_DESTROYRENDERTARGETVIEW",
    "PFND3D11_1DDI_CALCPRIVATESHADERSIZE",
    "PFND3D11_1DDI_CREATEVERTEXSHADER",
    "PFND3D11_1DDI_CREATEPIXELSHADER",
    "PFND3D10DDI_DESTROYSHADER",
    # The element-layout, binding and draw group.  PFND3D10DDI_SETSHADER is
    # one typedef over six slots -- every stage's SetShader takes the same
    # parameter list -- so this set of 14 names promotes 19 slots.
    "PFND3D10DDI_CALCPRIVATEELEMENTLAYOUTSIZE",
    "PFND3D10DDI_CREATEELEMENTLAYOUT",
    "PFND3D10DDI_DESTROYELEMENTLAYOUT",
    "PFND3D10DDI_SETINPUTLAYOUT",
    "PFND3D10DDI_IA_SETVERTEXBUFFERS",
    "PFND3D10DDI_IA_SETTOPOLOGY",
    "PFND3D10DDI_SETSHADER",
    "PFND3D11DDI_SETRENDERTARGETS",
    "PFND3D10DDI_SETVIEWPORTS",
    "PFND3D10DDI_CLEARRENDERTARGETVIEW",
    "PFND3D10DDI_DRAW",
    "PFND3D10DDI_SETBLENDSTATE",
    "PFND3D10DDI_SETDEPTHSTENCILSTATE",
    "PFND3D10DDI_SETRASTERIZERSTATE",
}

if not PROMOTED_SLOTS <= {type_name for _, type_name in DEVICEFUNC_SLOTS}:
    raise SystemExit(
        "PROMOTED_SLOTS names a typedef the device function table does not "
        "use: " + ", ".join(sorted(
            PROMOTED_SLOTS - {t for _, t in DEVICEFUNC_SLOTS})))

if len(DEVICEFUNC_SLOTS) != 178:
    raise SystemExit(
        "the device function table is published with 178 members, the model "
        f"lists {len(DEVICEFUNC_SLOTS)}"
    )

if len({type_name for _, type_name in DEVICEFUNC_SLOTS}) != 138:
    raise SystemExit(
        "the published device table must retain its 138 distinct PFN names"
    )

GROUPS = HANDLES + [
    ADAPTERFUNCS,
    ADAPTERFUNCS_2,
    OPENADAPTER,
    DXGI_BASE_ARGS,
    CREATEDEVICE,
    CREATECOMMANDLIST,
    CALCPRIVATEDEFERREDCONTEXTSIZE,
    HANDLESIZE,
    CREATEDEFERREDCONTEXT,
    MAPPED_SUBRESOURCE,
    CREATERESOURCE,
    CREATE11RESOURCE,
    OPENRESOURCE,
    BUFFER_SRV,
    TEX1D_SRV,
    TEX2D_SRV,
    TEX3D_SRV,
    TEXCUBE_SRV,
    BUFFEREX_SRV,
    CREATESHADERRESOURCEVIEW,
    BUFFER_RTV,
    TEX1D_RTV,
    TEX2D_RTV,
    TEX3D_RTV,
    TEXCUBE_RTV,
    CREATERENDERTARGETVIEW,
    DEVICEFUNCS,
    CORELAYER_CALLBACKS,
    KERNEL_CALLBACKS,
    ESCAPE,
    SYNCTOKEN,
]

# DXGI_DDI_BASE_ARGS is asserted in the header both on its own and through
# D3D10DDIARG_CREATEDEVICE's dotted paths.  Only the enclosing structure's
# assertions use the dotted form, so the model must not demand them twice.
DOTTED_OWNERS = {"D3D10DDIARG_CREATEDEVICE"}


def declare(type_name, name, indent, width):
    """One member, with the star on the name as the header writes it."""
    if type_name.endswith("*"):
        return f"{indent}{type_name.rstrip(' *'):<{width}} *{name};"
    return f"{indent}{type_name:<{width}} {name};"


def member_offset(member, fields):
    """Where a member of any kind starts, read back from the walk."""
    if isinstance(member, Union):
        return fields[member.arms[0].name]
    return fields[member.name]


def emit(struct):
    """The declaration and its assertion block, in the header's own style.

    Padding members are emitted at the derived gaps, so a group is authored
    padding-complete rather than passing the -Wpadded gate only after someone
    is told which bytes it missed.
    """
    fields, size, alignment = struct.walk()
    pads = {
        offset: f"WinePad{index}"
        for index, (offset, _) in enumerate(struct.padding())
    }
    lines = [f"typedef struct {struct.name}", "{"]

    def emit_pads_before(limit):
        for offset in sorted(pads):
            if offset < limit and pads[offset] not in emitted:
                lines.append(declare("UINT32", pads[offset], "    ", 30))
                emitted.add(pads[offset])

    emitted = set()
    for member in struct.members:
        emit_pads_before(member_offset(member, fields))
        if isinstance(member, Field):
            lines.append(declare(member.type_name, member.name, "    ", 30))
        elif isinstance(member, Union):
            lines.append("    union")
            lines.append("    {")
            for arm in member.arms:
                lines.append(declare(arm.type_name, arm.name, "        ", 26))
            lines.append("    };")
        elif isinstance(member, Embedded):
            lines.append(declare(member.struct.name, member.name, "    ", 30))
    emit_pads_before(size)

    lines += [f"}} {struct.name};", ""]
    lines.append(f"WINE_DDI_ASSERT_STANDARD_LAYOUT({struct.name});")
    lines.append(f"WINE_DDI_ASSERT_SIZE({struct.name}, {size});")
    lines.append(f"WINE_DDI_ASSERT_ALIGN({struct.name}, {alignment});")

    for name, offset in sorted(
            list(fields.items()) + [(pad, at) for at, pad in pads.items()],
            key=lambda entry: (entry[1], "." in entry[0])):
        if "." in name and struct.name not in DOTTED_OWNERS:
            continue
        lines.append(f"WINE_DDI_ASSERT_FIELD({struct.name}, {name}, {offset});")

    return lines


def parse_header(text):
    """Every single-line WINE_DDI_ASSERT_* in the header, by structure.

    Macro definitions are skipped: their third argument is the parameter name
    `expected`, not a number, but skipping #define lines keeps that an
    intention rather than an accident of the pattern.
    """
    fields = {}
    sizes = {}
    aligns = {}
    body = "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#define")
    )

    for type_name, name, offset in re.findall(
        r"WINE_DDI_ASSERT_FIELD\(\s*([\w ]+?)\s*,\s*([\w.]+)\s*,\s*(\d+)\s*\)", body
    ):
        fields.setdefault(type_name, {})[name] = int(offset)
    for type_name, size in re.findall(
        r"WINE_DDI_ASSERT_SIZE\(\s*([\w ]+?)\s*,\s*(\d+)\s*\)", body
    ):
        sizes[type_name] = int(size)
    for type_name, alignment in re.findall(
        r"WINE_DDI_ASSERT_ALIGN\(\s*([\w ]+?)\s*,\s*(\d+)\s*\)", body
    ):
        aligns[type_name] = int(alignment)

    return fields, sizes, aligns


def check(header=HEADER):
    errors = []

    # 1. Declaring a subset of a union's arms must not move anything.
    for struct in GROUPS:
        declared, declared_size, declared_align = struct.walk(published=False)
        published, published_size, published_align = struct.walk(published=True)
        if (declared_size, declared_align) != (published_size, published_align):
            errors.append(
                f"{struct.name}: declaring a subset of the published union "
                f"arms changes the structure from {published_size}/"
                f"{published_align} to {declared_size}/{declared_align} bytes"
            )
        for name, offset in declared.items():
            if name in published and published[name] != offset:
                errors.append(
                    f"{struct.name}.{name}: at {offset} as declared, "
                    f"{published[name]} as published"
                )

    # 2. The committed header must agree with the model, both ways.
    if not header.is_file():
        errors.append(f"{header}: not found; run this from the repository root")
        return errors

    asserted_fields, asserted_sizes, asserted_aligns = parse_header(
            header.read_text())

    # The device table deliberately preserves the published PFN names even
    # though all are placeholder pointers today. Layout assertions alone
    # cannot distinguish one pointer typedef from another, so validate the
    # typed member sequence and placeholder alias set explicitly.
    header_text = header.read_text()
    device_match = re.search(
        r"struct D3DWDDM2_6DDI_DEVICEFUNCS\s*\{(.*?)\};",
        header_text,
        re.DOTALL,
    )
    if device_match is None:
        errors.append("D3DWDDM2_6DDI_DEVICEFUNCS: declaration not found")
    else:
        declared_slots = re.findall(
            r"^\s*(PFN[A-Z0-9_]+)\s+(pfn[A-Za-z0-9_]+);\s*$",
            device_match.group(1),
            re.MULTILINE,
        )
        expected_slots = [
            (type_name, name) for name, type_name in DEVICEFUNC_SLOTS
        ]
        if declared_slots != expected_slots:
            errors.append(
                "D3DWDDM2_6DDI_DEVICEFUNCS: member PFN names or order "
                "differ from the documentation model"
            )

    declared_aliases = set(re.findall(
        r"typedef PFNWINE_D3D11DDI_UNDECLARED_CB\s+(PFN[A-Z0-9_]+);",
        header_text,
    ))
    expected_aliases = {
        type_name
        for _, type_name in DEVICEFUNC_SLOTS
        if type_name not in PROMOTED_SLOTS
    }
    missing_aliases = expected_aliases - declared_aliases
    if missing_aliases:
        errors.append(
            "D3DWDDM2_6DDI_DEVICEFUNCS: placeholder aliases missing for "
            + ", ".join(sorted(missing_aliases))
        )

    # Both directions.  A promoted slot still aliasing the placeholder is a
    # promotion that did not land; an unpromoted slot with a real signature is
    # one nobody recorded, and nothing else here would see either.
    still_placeholders = PROMOTED_SLOTS & declared_aliases
    if still_placeholders:
        errors.append(
            "D3DWDDM2_6DDI_DEVICEFUNCS: promoted slots are still placeholder "
            "aliases: " + ", ".join(sorted(still_placeholders))
        )

    declared_signatures = set(re.findall(
        r"typedef\s+\w+\s*\(\*(PFN[A-Z0-9_]+)\)\s*\(", header_text))
    unrecorded = (declared_signatures
                  & {type_name for _, type_name in DEVICEFUNC_SLOTS}
                  - PROMOTED_SLOTS)
    if unrecorded:
        errors.append(
            "D3DWDDM2_6DDI_DEVICEFUNCS: these slots have an authored "
            "signature but are not in PROMOTED_SLOTS: "
            + ", ".join(sorted(unrecorded))
        )
    unauthored = PROMOTED_SLOTS - declared_signatures
    if unauthored:
        errors.append(
            "D3DWDDM2_6DDI_DEVICEFUNCS: PROMOTED_SLOTS names slots with no "
            "authored signature: " + ", ".join(sorted(unauthored))
        )

    for struct in GROUPS:
        modelled, size, alignment = struct.walk()
        modelled = {
            name: offset
            for name, offset in modelled.items()
            if "." not in name or struct.name in DOTTED_OWNERS
        }
        asserted = asserted_fields.get(struct.name)

        if asserted is None:
            errors.append(
                f"{struct.name}: the model declares it but the header asserts "
                "no field offset for it"
            )
            continue

        if asserted_sizes.get(struct.name) != size:
            errors.append(
                f"{struct.name}: the model computes {size} bytes, the header "
                f"asserts {asserted_sizes.get(struct.name)}"
            )
        if asserted_aligns.get(struct.name) != alignment:
            errors.append(
                f"{struct.name}: the model computes alignment {alignment}, "
                f"the header asserts {asserted_aligns.get(struct.name)}"
            )

        for name, offset in modelled.items():
            if name not in asserted:
                errors.append(
                    f"{struct.name}.{name}: modelled at {offset} and not "
                    "asserted anywhere; an unasserted field is not acceptable"
                )
            elif asserted[name] != offset:
                errors.append(
                    f"{struct.name}.{name}: the model computes {offset}, the "
                    f"header asserts {asserted[name]}"
                )

        for name in asserted:
            if name not in modelled and not PAD_MEMBER.match(name):
                errors.append(
                    f"{struct.name}.{name}: asserted in the header but absent "
                    "from the model, so nothing derives its offset from the "
                    "specification"
                )

        errors.extend(check_padding(struct, asserted))

    return errors


# The header's name for a member that occupies padding the specification leaves
# anonymous.  It is this project's own device, so the model knows the naming
# convention rather than the names.
PAD_MEMBER = re.compile(r"^WinePad(\d+)$")


def check_padding(struct, asserted):
    """Every derived gap is named once, and every named pad sits in a gap.

    The model derives the gaps from the published member list; the header
    names them.  Neither side can move a pad without the other disagreeing,
    which is what keeps naming the bytes from becoming a way to assert
    whatever the header already says.
    """
    errors = []
    gaps = dict(struct.padding())
    pads = {
        name: offset
        for name, offset in asserted.items()
        if PAD_MEMBER.match(name)
    }

    for offset, size in sorted(gaps.items()):
        if offset not in pads.values():
            errors.append(
                f"{struct.name}: {size} byte(s) of padding at {offset} that no "
                "member names; add a WinePad member and assert it"
            )

    for name, offset in sorted(pads.items(), key=lambda pad: pad[1]):
        if offset not in gaps:
            errors.append(
                f"{struct.name}.{name}: asserted at {offset}, where the model "
                "derives no padding"
            )

    # Numbered in offset order, so the name of a pad is a fact about the
    # layout rather than about the order someone added them.
    expected = [
        f"WinePad{index}"
        for index, _ in enumerate(sorted(pads.values()))
    ]
    actual = [name for name, _ in sorted(pads.items(), key=lambda pad: pad[1])]
    if actual != expected:
        errors.append(
            f"{struct.name}: padding members are {actual}, expected "
            f"{expected} numbered in offset order"
        )

    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--check",
        action="store_true",
        help="verify the committed header against the model",
    )
    mode.add_argument(
        "--emit",
        nargs="*",
        metavar="STRUCT",
        help="print declarations and assertions, all groups if none named",
    )
    parser.add_argument(
        "--header",
        type=pathlib.Path,
        default=HEADER,
        help=f"the header to check against (default: {HEADER})",
    )
    args = parser.parse_args()

    if args.check:
        errors = check(args.header)
        if errors:
            for error in errors:
                print(error, file=sys.stderr)
            return 1
        total = sum(len(struct.walk()[0]) for struct in GROUPS)
        print(
            f"ddi layout model: {len(GROUPS)} structures and {total} fields "
            "agree with the header"
        )
        return 0

    wanted = set(args.emit or [])
    for struct in GROUPS:
        if wanted and struct.name not in wanted:
            continue
        print("\n".join(emit(struct)))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
