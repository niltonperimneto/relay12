#!/usr/bin/env python3
"""Verify the pinned Wine D3D11 frontend exposes its backend lifecycle seam."""

import argparse
import pathlib


REQUIRED_HEADER = (
    "struct d3d11_backend_ops",
    "void (*destroy_device)(struct d3d_device *device);",
    "void (*flush)(struct d3d11_device_context *context);",
    "void (*draw)(struct d3d11_device_context *context, UINT vertex_count,",
    "void (*draw_indexed)(struct d3d11_device_context *context,",
    "void (*draw_instanced)(struct d3d11_device_context *context,",
    "void (*draw_indexed_instanced)(struct d3d11_device_context *context,",
    "void (*set_primitive_topology)(struct d3d11_device_context *context,",
    "HRESULT (*create_buffer)(struct d3d_device *device,",
    "void (*destroy_buffer)(struct d3d_device *device,",
    "void (*set_vertex_buffers)(struct d3d11_device_context *context,",
    "void (*set_index_buffer)(struct d3d11_device_context *context,",
    "HRESULT (*create_input_layout)(struct d3d_device *device,",
    "void (*destroy_input_layout)(struct d3d_device *device,",
    "void (*set_input_layout)(struct d3d11_device_context *context,",
    "HRESULT (*create_vertex_shader)(struct d3d_device *device,",
    "HRESULT (*create_pixel_shader)(struct d3d_device *device,",
    "void (*destroy_vertex_shader)(struct d3d_device *device,",
    "void (*destroy_pixel_shader)(struct d3d_device *device,",
    "void (*set_vertex_shader)(struct d3d11_device_context *context,",
    "void (*set_pixel_shader)(struct d3d11_device_context *context,",
    "const struct d3d11_backend_ops *backend_ops;",
    "void *backend_private;",
    "BOOL standalone_allocation;",
    "ID3D11On12Device1 ID3D11On12Device1_iface;",
    "HRESULT (*create_wrapped_resource)(struct d3d_device *device,",
    "void (*release_wrapped_resources)(struct d3d_device *device,",
    "void (*acquire_wrapped_resources)(struct d3d_device *device,",
    "HRESULT (*get_d3d12_device)(struct d3d_device *device, REFIID iid,",
)

REQUIRED_DEVICE = (
    "static const struct d3d11_backend_ops wined3d_backend_ops",
    "context->device->backend_ops->flush(context);",
    "context->device->backend_ops->draw(context, vertex_count,",
    "context->device->backend_ops->draw_indexed(context, index_count,",
    "context->device->backend_ops->draw_instanced(context,",
    "context->device->backend_ops->draw_indexed_instanced(context,",
    "context->device->backend_ops->set_primitive_topology(context, topology);",
    "context->device->backend_ops->set_vertex_buffers(context, start_slot,",
    "context->device->backend_ops->set_index_buffer(context, buffer, format,",
    "context->device->backend_ops->set_input_layout(context, input_layout);",
    "device->backend_ops->set_input_layout(&device->immediate_context,",
    "context->device->backend_ops->set_vertex_shader(context, shader);",
    "context->device->backend_ops->set_pixel_shader(context, shader);",
    "device->backend_ops->get_feature_level(device)",
    "device->backend_ops->get_creation_flags(device)",
    "device->backend_ops->get_device_removed_reason(device)",
    "device->backend_ops->destroy_device(device);",
    "device->backend_ops = &wined3d_backend_ops;",
    "|| !backend_ops->create_input_layout",
    "|| !backend_ops->destroy_input_layout",
    "|| !backend_ops->set_input_layout",
    "|| !backend_ops->create_vertex_shader",
    "|| !backend_ops->create_pixel_shader",
    "|| !backend_ops->destroy_vertex_shader",
    "|| !backend_ops->destroy_pixel_shader",
    "|| !backend_ops->set_vertex_shader",
    "|| !backend_ops->set_pixel_shader",
    "static const struct ID3D11On12Device1Vtbl d3d11_on12_device_vtbl",
    "return IUnknown_QueryInterface(device->outer_unk, iid, out);",
    "*out = &device->ID3D11On12Device1_iface;",
    "device->ID3D11On12Device1_iface.lpVtbl = &d3d11_on12_device_vtbl;",
    "struct d3d_device *d3d_device_create_backend(",
    "d3d_device_init(device, &device->IUnknown_inner);",
    "if (device->standalone_allocation)",
    "device->d3d11_only = TRUE;\n    return device;\n}",
)

REQUIRED_BUFFER = (
    "device->backend_ops->get_feature_level(device)",
    "device_impl->backend_ops->destroy_buffer(device_impl, buffer);",
    "device->backend_ops->create_buffer(device, desc, data,",
    "buffer->backend_buffer.size = sizeof(buffer->backend_buffer);",
)

REQUIRED_SHADER = (
    "device->backend_ops->create_vertex_shader(device,",
    "device_impl->backend_ops->destroy_vertex_shader(device_impl, shader);",
    "device->backend_ops->create_pixel_shader(device,",
    "device_impl->backend_ops->destroy_pixel_shader(device_impl, shader);",
    "shader->backend_shader.size = sizeof(shader->backend_shader);",
)

REQUIRED_MAIN = (
    "static const struct d3d11_backend_ops d3d11_on12_backend_ops",
    "WineD3D11On12OpenAdapterV1",
    "d3d_device_create_backend(&d3d11_on12_backend_ops,",
    "IUnknown_Release(&d3d_device->IUnknown_inner);",
    "backend->core.flush_adapter_device(&backend->adapter, 0, 0,",
    "backend->core.draw_adapter_device(&backend->adapter,",
    "backend->core.dispatch_draw(&backend->adapter, kind, count0,",
    "backend->core.set_primitive_topology(&backend->adapter,",
    "backend->create_buffer(&backend->adapter, desc, data,",
    "backend->set_vertex_buffers(&backend->adapter, start_slot,",
    "backend->set_index_buffer(&backend->adapter,",
    "backend->create_input_layout(&backend->adapter, elements, registers,",
    "backend->destroy_input_layout(",
    "backend->set_input_layout(&backend->adapter,",
    "backend->create_vertex_shader(&backend->adapter, byte_code,",
    "backend->create_pixel_shader(&backend->adapter, byte_code,",
    "backend->destroy_shader((struct wine_d3d11on12_shader *)",
    "backend->set_vertex_shader(&backend->adapter,",
    "backend->set_pixel_shader(&backend->adapter,",
    '"WineD3D11On12CreateVertexShaderV1"',
    '"WineD3D11On12CreatePixelShaderV1"',
    '"WineD3D11On12DestroyShaderV1"',
    '"WineD3D11On12SetVertexShaderV1"',
    '"WineD3D11On12SetPixelShaderV1"',
)


def check_tree(root):
    root = pathlib.Path(root)
    header = (root / "dlls/d3d11/d3d11_private.h").read_text()
    device = (root / "dlls/d3d11/device.c").read_text()
    buffer = (root / "dlls/d3d11/buffer.c").read_text()
    shader = (root / "dlls/d3d11/shader.c").read_text()
    main = (root / "dlls/d3d11/d3d11_main.c").read_text()
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
    for marker in REQUIRED_BUFFER:
        if marker not in buffer:
            errors.append(f"buffer.c is missing: {marker}")
    for marker in REQUIRED_SHADER:
        if marker not in shader:
            errors.append(f"shader.c is missing: {marker}")
    for marker in REQUIRED_MAIN:
        if marker not in main:
            errors.append(f"d3d11_main.c is missing: {marker}")

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
