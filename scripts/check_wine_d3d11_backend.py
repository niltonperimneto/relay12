#!/usr/bin/env python3
"""Verify the pinned Wine D3D11 frontend exposes its backend lifecycle seam."""

import argparse
import pathlib


REQUIRED_HEADER = (
    "struct d3d11_backend_ops",
    "void (*destroy_device)(struct d3d_device *device);",
    "void (*flush)(struct d3d11_device_context *context);",
    "const struct d3d11_backend_ops *backend_ops;",
    "void *backend_private;",
    "ID3D11On12Device1 ID3D11On12Device1_iface;",
    "HRESULT (*create_wrapped_resource)(struct d3d_device *device,",
    "void (*release_wrapped_resources)(struct d3d_device *device,",
    "void (*acquire_wrapped_resources)(struct d3d_device *device,",
    "HRESULT (*get_d3d12_device)(struct d3d_device *device, REFIID iid,",
)

REQUIRED_DEVICE = (
    "static const struct d3d11_backend_ops wined3d_backend_ops",
    "context->device->backend_ops->flush(context);",
    "device->backend_ops->get_feature_level(device)",
    "device->backend_ops->get_creation_flags(device)",
    "device->backend_ops->get_device_removed_reason(device)",
    "device->backend_ops->destroy_device(device);",
    "device->backend_ops = &wined3d_backend_ops;",
    "static const struct ID3D11On12Device1Vtbl d3d11_on12_device_vtbl",
    "return IUnknown_QueryInterface(device->outer_unk, iid, out);",
    "*out = &device->ID3D11On12Device1_iface;",
    "device->ID3D11On12Device1_iface.lpVtbl = &d3d11_on12_device_vtbl;",
)


def check_tree(root):
    root = pathlib.Path(root)
    header = (root / "dlls/d3d11/d3d11_private.h").read_text()
    device = (root / "dlls/d3d11/device.c").read_text()
    configure = (root / "configure.ac").read_text()
    host_makefile = root / "dlls/d3d11on12host/Makefile.in"
    host_spec = root / "dlls/d3d11on12host/d3d11on12host.spec"
    errors = []

    for marker in REQUIRED_HEADER:
        if marker not in header:
            errors.append(f"d3d11_private.h is missing: {marker}")
    for marker in REQUIRED_DEVICE:
        if marker not in device:
            errors.append(f"device.c is missing: {marker}")

    if "WINE_CONFIG_MAKEFILE(dlls/d3d11on12host)" not in configure:
        errors.append("configure.ac does not configure d3d11on12host")
    if not host_makefile.is_file():
        errors.append("d3d11on12host/Makefile.in is missing")
    elif "MODULE    = d3d11on12host.dll" not in host_makefile.read_text():
        errors.append("d3d11on12host has the wrong module name")
    if not host_spec.is_file():
        errors.append("d3d11on12host/d3d11on12host.spec is missing")
    elif "D3D11On12CreateDevice" not in host_spec.read_text():
        errors.append("d3d11on12host does not export D3D11On12CreateDevice")

    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wine_tree", type=pathlib.Path)
    args = parser.parse_args()

    errors = check_tree(args.wine_tree)
    if errors:
        for error in errors:
            print(error)
        return 1

    print("Wine D3D11 backend lifecycle gate: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
