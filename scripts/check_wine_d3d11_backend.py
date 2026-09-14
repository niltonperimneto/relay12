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
)

REQUIRED_DEVICE = (
    "static const struct d3d11_backend_ops wined3d_backend_ops",
    "context->device->backend_ops->flush(context);",
    "device->backend_ops->get_feature_level(device)",
    "device->backend_ops->get_creation_flags(device)",
    "device->backend_ops->get_device_removed_reason(device)",
    "device->backend_ops->destroy_device(device);",
    "device->backend_ops = &wined3d_backend_ops;",
)


def check_tree(root):
    root = pathlib.Path(root)
    header = (root / "dlls/d3d11/d3d11_private.h").read_text()
    device = (root / "dlls/d3d11/device.c").read_text()
    errors = []

    for marker in REQUIRED_HEADER:
        if marker not in header:
            errors.append(f"d3d11_private.h is missing: {marker}")
    for marker in REQUIRED_DEVICE:
        if marker not in device:
            errors.append(f"device.c is missing: {marker}")

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
