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
        fields = {}
        offset = 0
        alignment = 1

        for member in self.members:
            if isinstance(member, Field):
                offset = align_up(offset, member.align)
                fields[member.name] = offset
                offset += member.size
                alignment = max(alignment, member.align)
            elif isinstance(member, Union):
                arms = member.members_for(published)
                arm_align = max(arm.align for arm in arms)
                arm_size = max(arm.size for arm in arms)
                offset = align_up(offset, arm_align)
                for arm in arms:
                    fields[arm.name] = offset
                offset += arm_size
                alignment = max(alignment, arm_align)
            elif isinstance(member, Embedded):
                inner, inner_size, inner_align = member.struct.walk(published)
                offset = align_up(offset, inner_align)
                fields[member.name] = offset
                for inner_name, inner_offset in inner.items():
                    fields[f"{member.name}.{inner_name}"] = offset + inner_offset
                offset += inner_size
                alignment = max(alignment, inner_align)
            else:
                raise TypeError(f"unknown member kind: {member!r}")

        return fields, align_up(offset, alignment), alignment


# --- The declaration groups -------------------------------------------------
#
# Group: driver and runtime object handles
# Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/direct3d-version-10-runtime-and-driver-handles
# Retrieved: 2026-09-06
#
# A handle is one wrapped pointer.  The driver handle points at the
# runtime-allocated private block, so its member is pDrvPrivate; the runtime
# handle carries an opaque runtime value, so its member is handle.

DRIVER_HANDLES = ["D3D10DDI_HADAPTER", "D3D10DDI_HRESOURCE", "D3D10DDI_HDEVICE"]
RUNTIME_HANDLES = [
    "D3D10DDI_HRTADAPTER",
    "D3D10DDI_HRTRESOURCE",
    "D3D10DDI_HRTDEVICE",
    "D3D10DDI_HRTCORELAYER",
]

HANDLES = [
    Struct(name, [Field("pDrvPrivate", "void *")]) for name in DRIVER_HANDLES
] + [Struct(name, [Field("handle", "void *")]) for name in RUNTIME_HANDLES]


def handle(name):
    return (name, POINTER[0], POINTER[1])


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

GROUPS = HANDLES + [
    ADAPTERFUNCS,
    ADAPTERFUNCS_2,
    OPENADAPTER,
    DXGI_BASE_ARGS,
    CREATEDEVICE,
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


def emit(struct):
    """The declaration and its assertion block, in the header's own style."""
    fields, size, alignment = struct.walk()
    lines = [f"typedef struct {struct.name}", "{"]

    for member in struct.members:
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

    lines += [f"}} {struct.name};", ""]
    lines.append(f"WINE_DDI_ASSERT_STANDARD_LAYOUT({struct.name});")
    lines.append(f"WINE_DDI_ASSERT_SIZE({struct.name}, {size});")
    lines.append(f"WINE_DDI_ASSERT_ALIGN({struct.name}, {alignment});")

    for name, offset in fields.items():
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
            if name not in modelled:
                errors.append(
                    f"{struct.name}.{name}: asserted in the header but absent "
                    "from the model, so nothing derives its offset from the "
                    "specification"
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
