#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# Tests for the checks that guard the D3D11On12 work.
#
# These gates used to be heredocs inside .github/workflows/pull-request.yml,
# where nothing could run them but CI and nothing could test them at all.  A
# gate with no test is a gate that passes everything from the day its pattern
# stops matching, and it fails silently: the run stays green, which is exactly
# the signal it exists to withhold.
#
# So each gate is tested for both answers.  Asserting that a correct tree
# passes proves nothing on its own -- an empty check does that too -- so every
# rule a gate claims to enforce is given something that breaks it.
#
# Run: python3 -m unittest discover -s tests -p 'test_*.py'

import pathlib
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPOSITORY / "scripts"))

import check_ddi_header  # noqa: E402
import check_d3d11on12_port  # noqa: E402
import check_wine_d3d11_backend  # noqa: E402
import check_interface_acquisition  # noqa: E402
import check_pe_audit  # noqa: E402
import check_shared_state  # noqa: E402
import gen_ddi_layout  # noqa: E402
import check_dtl_struct_return  # noqa: E402
import check_dtl_include_case  # noqa: E402
import check_adapter_args  # noqa: E402
import check_core_callbacks  # noqa: E402
import check_cleanroom_isolation  # noqa: E402
import inventory_dtl_portability  # noqa: E402

DDI_HEADER = REPOSITORY / "relay12-d3d11" / "ddi" / "wine_d3d11ddi.h"

GOOD_GROUP = """\
/*
 * Group: a well formed group
 * Specification: https://learn.microsoft.com/en-us/example
 * Retrieved: 2026-09-07
 */
typedef struct example { void *field; } example;
"""


def written(text, suffix=".h"):
    """A temporary file holding text, returned as a path."""
    handle = tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False)
    with handle:
        handle.write(text)
    return pathlib.Path(handle.name)


class WineD3D11BackendGate(unittest.TestCase):
    def make_tree(self, header, device):
        temporary = tempfile.TemporaryDirectory()
        root = pathlib.Path(temporary.name)
        source = root / "dlls" / "d3d11"
        source.mkdir(parents=True)
        (source / "d3d11_private.h").write_text(header)
        (source / "device.c").write_text(device)
        (source / "buffer.c").write_text("\n".join(
            check_wine_d3d11_backend.REQUIRED_BUFFER))
        (source / "shader.c").write_text("\n".join(
            check_wine_d3d11_backend.REQUIRED_SHADER))
        (source / "texture.c").write_text("\n".join(
            check_wine_d3d11_backend.REQUIRED_TEXTURE))
        (source / "d3d11_main.c").write_text("\n".join(
            check_wine_d3d11_backend.REQUIRED_MAIN))
        (root / "configure.ac").write_text(
            "WINE_CONFIG_MAKEFILE(dlls/d3d11on12host)")
        host = root / "dlls" / "d3d11on12host"
        host.mkdir()
        (host / "Makefile.in").write_text(
            "MODULE    = d3d11on12host.dll")
        (host / "d3d11on12host.spec").write_text(
            "@ stdcall D3D11On12CreateDevice()")
        self.addCleanup(temporary.cleanup)
        return root

    def test_complete_lifecycle_seam_passes(self):
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(
            check_wine_d3d11_backend.REQUIRED_DEVICE))
        self.assertEqual(check_wine_d3d11_backend.check_tree(root), [])

    def test_direct_flush_regression_is_rejected(self):
        markers = list(check_wine_d3d11_backend.REQUIRED_DEVICE)
        markers.remove("context->device->backend_ops->flush(context);")
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(markers))
        errors = check_wine_d3d11_backend.check_tree(root)
        self.assertTrue(any("backend_ops->flush" in error for error in errors))

    def test_missing_separate_host_module_is_rejected(self):
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(
            check_wine_d3d11_backend.REQUIRED_DEVICE))
        (root / "dlls/d3d11on12host/Makefile.in").unlink()
        errors = check_wine_d3d11_backend.check_tree(root)
        self.assertTrue(any("Makefile.in is missing" in error for error in errors))

    def test_on12_dxgi_device_regression_is_rejected(self):
        # Without the IDXGIDevice branch Unity's D3D12 renderer discards the
        # On12 device and falls back to D3D11, as PEAK did before patch 0025.
        markers = list(check_wine_d3d11_backend.REQUIRED_DEVICE)
        markers.remove("*out = &device->IDXGIDevice_iface;")
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(markers))
        errors = check_wine_d3d11_backend.check_tree(root)
        self.assertTrue(any("IDXGIDevice_iface" in error for error in errors))

    def test_on12_private_store_leak_is_rejected(self):
        markers = list(check_wine_d3d11_backend.REQUIRED_DEVICE)
        markers.remove("wined3d_private_store_cleanup(&device->private_store);")
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(markers))
        errors = check_wine_d3d11_backend.check_tree(root)
        self.assertTrue(any("private_store_cleanup" in error for error in errors))

    def test_shader_lifecycle_regression_is_rejected(self):
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(
            check_wine_d3d11_backend.REQUIRED_DEVICE))
        shader = root / "dlls/d3d11/shader.c"
        shader.write_text(shader.read_text().replace(
            "device_impl->backend_ops->destroy_vertex_shader(device_impl, shader);",
            ""))
        errors = check_wine_d3d11_backend.check_tree(root)
        self.assertTrue(any("destroy_vertex_shader" in error for error in errors))


    def test_texture2d_lifecycle_regression_is_rejected(self):
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(
            check_wine_d3d11_backend.REQUIRED_DEVICE))
        texture = root / "dlls/d3d11/texture.c"
        texture.write_text(texture.read_text().replace(
            "device_impl->backend_ops->destroy_texture2d(device_impl, texture);",
            ""))
        errors = check_wine_d3d11_backend.check_tree(root)
        self.assertTrue(any("destroy_texture2d" in error for error in errors))

    def test_positional_wined3d_backend_table_is_rejected(self):
        """The table must stay designated.

        A positional table already shifted every WineD3D entry after
        destroy_buffer once, when create_texture2d and destroy_texture2d were
        added to the ops struct. The gate has to catch a return to that form.
        """
        markers = list(check_wine_d3d11_backend.REQUIRED_DEVICE)
        markers.remove(".set_vertex_shader = wined3d_backend_set_vertex_shader,")
        root = self.make_tree("\n".join(
            check_wine_d3d11_backend.REQUIRED_HEADER), "\n".join(markers))
        errors = check_wine_d3d11_backend.check_tree(root)
        self.assertTrue(any("set_vertex_shader" in error for error in errors))


class D3D11On12PortGate(unittest.TestCase):
    def test_portable_source_passes(self):
        source = "RelayComPtr<IUnknown> pointer; // _com_error in a comment"
        self.assertEqual(check_d3d11on12_port.check_source("good.cpp", source), [])

    def test_executable_com_error_is_rejected(self):
        errors = check_d3d11on12_port.check_source(
            "bad.cpp", "throw _com_error(E_FAIL);")
        self.assertTrue(any("MSVC _com_error" in error for error in errors))

    def test_executable_atl_pointer_is_rejected(self):
        errors = check_d3d11on12_port.check_source(
            "bad.hpp", "CComPtr<IUnknown> pointer;")
        self.assertTrue(any("ATL CComPtr" in error for error in errors))

    def test_backslash_include_is_rejected(self):
        errors = check_d3d11on12_port.check_source(
            "bad.cpp", "#include <dxc\\dxcapi.h>")
        self.assertTrue(any("Windows separator" in error for error in errors))

    def test_executable_atl_heap_pointer_is_rejected(self):
        errors = check_d3d11on12_port.check_source(
            "bad.cpp", "CComHeapPtr<void> allocation;")
        self.assertTrue(any("ATL CComHeapPtr" in error for error in errors))

    def test_comment_only_mentions_do_not_trip_the_gate(self):
        source = "// CComPtr<IUnknown> was removed\nint value; // _com_error"
        self.assertEqual(check_d3d11on12_port.check_source("notes.cpp", source), [])

    IMM_CTX_ARGS = """\
    inline D3D12TranslationLayer::ImmediateContext::CreationArgs GetImmCtxArgs(Adapter* pAdapter, UINT CreateDeviceFlags)
    {
        D3D12TranslationLayer::ImmediateContext::CreationArgs args = {};
        args.UseResidencyManagement = true;
%s        return args;
    }
"""

    def test_upload_submit_limit_is_required(self):
        errors = check_d3d11on12_port.check_immediate_context_args(
            "device.cpp", self.IMM_CTX_ARGS % "")
        self.assertTrue(any("patch 0025" in error for error in errors))

    def test_zero_upload_submit_limit_is_rejected(self):
        errors = check_d3d11on12_port.check_immediate_context_args(
            "device.cpp", self.IMM_CTX_ARGS
            % "        args.MaxAllocatedUploadHeapSpacePerCommandList = 0u;\n")
        self.assertTrue(any("patch 0025" in error for error in errors))

    UPLOAD_LIMIT = "        args.MaxAllocatedUploadHeapSpacePerCommandList = MAXDWORD;\n"
    NON_BLOCKING = ("        args.UseNonBlockingPSOs = GetCompatValue("
                    "\"NonBlockingPSOs\", &nonBlockingPSOs) && nonBlockingPSOs;\n")

    def test_upload_submit_limit_passes(self):
        self.assertEqual(check_d3d11on12_port.check_immediate_context_args(
            "device.cpp",
            self.IMM_CTX_ARGS % (self.UPLOAD_LIMIT + self.NON_BLOCKING)), [])

    def test_non_blocking_pso_switch_is_required(self):
        errors = check_d3d11on12_port.check_immediate_context_args(
            "device.cpp", self.IMM_CTX_ARGS % self.UPLOAD_LIMIT)
        self.assertEqual(len(errors), 1)
        self.assertIn("patch 0027", errors[0])

    COMPAT_VALUE = """\
bool GetCompatValue(const char* str, UINT64* pValue)
{
    if (pfnCompatValue)
    {
        return pfnCompatValue(str, pValue);
    }
%s    return false;
}
"""

    def test_compat_value_environment_fallback_is_required(self):
        errors = check_d3d11on12_port.check_compat_value(
            "main.cpp", self.COMPAT_VALUE % "")
        self.assertTrue(any("patch 0026" in error for error in errors))

    def test_compat_value_environment_fallback_passes(self):
        fallback = ('    snprintf(name, sizeof(name), "D3D11ON12_COMPAT_%s", str);\n'
                    "    GetEnvironmentVariableA(name, value, sizeof(value));\n")
        self.assertEqual(check_d3d11on12_port.check_compat_value(
            "main.cpp", self.COMPAT_VALUE % fallback), [])

    PRE_DRAW_DISPATCH = """\
inline void ImmediateContext::PreDraw() noexcept(false)
{
        auto pPSO = bNonBlocking
            ? m_CurrentState.m_pPSO->%s(COMMAND_LIST_TYPE::GRAPHICS)
            : m_CurrentState.m_pPSO->GetForUse(COMMAND_LIST_TYPE::GRAPHICS);
}

inline void ImmediateContext::PreDispatch() noexcept(false)
{
            auto pPSO = m_CurrentState.m_pPSO->%s(COMMAND_LIST_TYPE::GRAPHICS);
}
"""

    def test_draws_may_skip_a_compiling_pso(self):
        self.assertEqual(check_d3d11on12_port.check_dtl_pso_lookup(
            "ImmediateContext.inl",
            self.PRE_DRAW_DISPATCH % ("TryGetForUse", "GetForUse")), [])

    def test_draws_that_always_wait_are_rejected(self):
        errors = check_d3d11on12_port.check_dtl_pso_lookup(
            "ImmediateContext.inl",
            self.PRE_DRAW_DISPATCH % ("GetForUse", "GetForUse"))
        self.assertTrue(any("dtl patch 0023" in error for error in errors))

    def test_compute_that_skips_a_compiling_pso_is_rejected(self):
        errors = check_d3d11on12_port.check_dtl_pso_lookup(
            "ImmediateContext.inl",
            self.PRE_DRAW_DISPATCH % ("TryGetForUse", "TryGetForUse"))
        self.assertTrue(any("PreDispatch must never skip" in error
                            for error in errors))

    def test_internal_pipelines_must_not_skip(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "include").mkdir()
            (root / "src").mkdir()
            (root / "include" / "ImmediateContext.inl").write_text(
                self.PRE_DRAW_DISPATCH % ("TryGetForUse", "GetForUse"))
            (root / "src" / "BlitHelper.cpp").write_text(
                "pCommandList->SetPipelineState("
                "pPSO->TryGetForUse(COMMAND_LIST_TYPE::GRAPHICS));\n")
            errors = check_d3d11on12_port.check_dtl_tree(root)
        self.assertEqual(len(errors), 1)
        self.assertIn("BlitHelper.cpp", errors[0])

    def test_the_non_blocking_pso_patches_are_in_the_series(self):
        for patch in ("patches/dtl/0023-add-non-blocking-pso-lookup.patch",
                      "patches/d3d11on12/0026-read-compat-values-from-environment.patch",
                      "patches/d3d11on12/0027-enable-non-blocking-psos.patch"):
            self.assertTrue((REPOSITORY / patch).is_file(), patch)

    def test_the_pinned_dtl_tree_without_the_series_is_rejected(self):
        inline = (REPOSITORY / "third_party" / "D3D12TranslationLayer"
                  / "include" / "ImmediateContext.inl")
        if not inline.exists():
            self.skipTest("D3D12TranslationLayer submodule not checked out")
        errors = check_d3d11on12_port.check_dtl_pso_lookup(
            inline, inline.read_text(errors="replace"))
        self.assertTrue(any("dtl patch 0023" in error for error in errors))

    def test_the_pinned_tree_without_the_series_is_rejected(self):
        device = REPOSITORY / "third_party" / "D3D11On12" / "src" / "device.cpp"
        if not device.exists():
            self.skipTest("D3D11On12 submodule not checked out")
        errors = check_d3d11on12_port.check_immediate_context_args(
            device, device.read_text(errors="replace"))
        self.assertTrue(any("patch 0025" in error for error in errors))



DRIVER_ADAPTER_ARGS = """\
struct PrivateCallbacks
{
    D3D11_RESOURCE_FLAGS (CALLBACK *GetResourceFlags)(_In_ D3D10DDI_HRESOURCE, _Out_ bool *pbAcquireableOnWrite);
    bool (CALLBACK *NotifySharedResourceCreation)(_In_ HANDLE, _In_ IUnknown*);
};

struct PrivateCallbacks2
{
    HRESULT(CALLBACK* Present11On12CB)(_In_ HANDLE, _In_ Present11On12CBArgs*);
};

constexpr UINT c_CurrentD3D11On12InterfaceVersion = 7;

struct SOpenAdapterArgs
{
    ID3D12Device1* pDevice;
    UINT NodeIndex;
    PrivateCallbacks Callbacks;
    // Velocity features
    bool bSupportDeferredContexts;
    UINT D3D11On12InterfaceVersion = c_CurrentD3D11On12InterfaceVersion;
    PrivateCallbacks2* Callbacks2;
};
"""

# The same declarations as a GPL transcription would spell them: no SAL, a
# different pointer style, wrapped differently, and the forward declaration
# the core needs because it does not include the driver's header.
CORE_ADAPTER_ARGS = """\
struct PrivateCallbacks
{
    D3D11_RESOURCE_FLAGS (CALLBACK *GetResourceFlags)(D3D10DDI_HRESOURCE,
            bool *pbAcquireableOnWrite);
    bool (CALLBACK *NotifySharedResourceCreation)(HANDLE, IUnknown *);
};

struct Present11On12CBArgs;

struct PrivateCallbacks2
{
    HRESULT (CALLBACK *Present11On12CB)(HANDLE, Present11On12CBArgs *);
};

constexpr UINT c_CurrentD3D11On12InterfaceVersion = 7;

struct SOpenAdapterArgs
{
    ID3D12Device1 *pDevice;
    UINT NodeIndex;
    PrivateCallbacks Callbacks;
    bool bSupportDeferredContexts;
    UINT D3D11On12InterfaceVersion = c_CurrentD3D11On12InterfaceVersion;
    PrivateCallbacks2 *Callbacks2;
};
"""


class ShaderInterfaceTranscription(unittest.TestCase):
    """The shader vtable prefix and SHADER_DESC are transcribed, not included.

    Both are read by address across a module boundary, so a reordered slot or
    a retyped field would not fail to compile here -- the driver would call
    the wrong method or read the wrong offset.  Each mutation is asserted to
    have changed the header, because a `replace` that matched nothing would
    otherwise be indistinguishable from a gate that caught it.
    """

    DRIVER = REPOSITORY / "third_party/D3D11On12/interface/D3D11On12DDI.h"
    HEADER = REPOSITORY / "relay12-d3d11/wine_d3d11on12_shader.h"

    def setUp(self):
        # errors="replace" to match how the gate itself reads the pinned MIT
        # header, which is not this project's file and not guaranteed UTF-8.
        self.driver = self.DRIVER.read_text(encoding="utf-8", errors="replace")
        self.header = self.HEADER.read_text(encoding="utf-8")

    def test_the_committed_header_passes(self):
        self.assertEqual(
            check_adapter_args.check_shader_interface(self.driver, self.header),
            [])

    def test_a_reordered_vtable_slot_is_rejected(self):
        mutation = self.header.replace("*Signal", "*WrongSlot")
        self.assertNotEqual(mutation, self.header)
        self.assertTrue(
            any("vtable prefix differs" in error for error in
                check_adapter_args.check_shader_interface(
                    self.driver, mutation)))

    def test_a_reordered_resource_vtable_is_rejected(self):
        mutation = self.header.replace("*SetGraphicsCurrentState", "*WrongResourceSlot")
        self.assertNotEqual(mutation, self.header)
        self.assertTrue(any("resource interface: vtable order" in error for error in
            check_adapter_args.check_shader_interface(self.driver, mutation)))

    def test_a_retyped_descriptor_field_is_rejected(self):
        mutation = self.header.replace("UINT SizeInBytes", "SIZE_T SizeInBytes")
        self.assertNotEqual(mutation, self.header)
        self.assertTrue(
            any("SHADER_DESC layout differs" in error for error in
                check_adapter_args.check_shader_interface(
                    self.driver, mutation)))


class AdapterArgsTranscription(unittest.TestCase):
    """The copy in the core is compared to the pinned driver on every run.

    It is passed by address to separately compiled code, so a field added
    upstream would not fail to compile here -- the driver would read past the
    end of a structure this side believes it filled.
    """

    def test_a_faithful_transcription_passes_despite_spelling(self):
        self.assertEqual(
            check_adapter_args.check(DRIVER_ADAPTER_ARGS, CORE_ADAPTER_ARGS),
            [])

    def test_the_committed_transcription_matches_the_pinned_driver(self):
        driver = (REPOSITORY / "third_party" / "D3D11On12" / "interface"
                  / "D3D11On12DDI.h")
        core = REPOSITORY / "relay12-d3d11" / "d3d11on12core.cpp"
        if not driver.exists():
            self.skipTest("the pinned driver submodule is not checked out")
        self.assertEqual(
            check_adapter_args.check(driver.read_text(errors="replace"),
                                     core.read_text(errors="replace")),
            [])

    def test_a_field_added_upstream_is_rejected(self):
        """The failure this gate exists for: the driver grows a member and
        the core keeps passing the shorter structure."""
        grown = DRIVER_ADAPTER_ARGS.replace(
            "    PrivateCallbacks2* Callbacks2;",
            "    PrivateCallbacks2* Callbacks2;\n"
            "    bool bSupportPrepatchedShaders;")
        self.assertNotEqual(grown, DRIVER_ADAPTER_ARGS)
        errors = check_adapter_args.check(grown, CORE_ADAPTER_ARGS)
        self.assertTrue(any("SOpenAdapterArgs" in error for error in errors),
                        errors)

    def test_a_reordered_field_is_rejected(self):
        """Names alone would not catch this; order is what decides offsets."""
        swapped = CORE_ADAPTER_ARGS.replace(
            "    ID3D12Device1 *pDevice;\n    UINT NodeIndex;",
            "    UINT NodeIndex;\n    ID3D12Device1 *pDevice;")
        self.assertNotEqual(swapped, CORE_ADAPTER_ARGS)
        errors = check_adapter_args.check(DRIVER_ADAPTER_ARGS, swapped)
        self.assertTrue(any("SOpenAdapterArgs" in error for error in errors),
                        errors)

    def test_a_retyped_field_is_rejected(self):
        retyped = CORE_ADAPTER_ARGS.replace("    UINT NodeIndex;",
                                            "    UINT64 NodeIndex;")
        self.assertNotEqual(retyped, CORE_ADAPTER_ARGS)
        errors = check_adapter_args.check(DRIVER_ADAPTER_ARGS, retyped)
        self.assertTrue(any("NodeIndex" in error for error in errors), errors)

    def test_a_changed_interface_version_is_rejected(self):
        """Version 7 is where the driver starts dereferencing Callbacks2, so
        a bump is a crash risk rather than a number."""
        bumped = DRIVER_ADAPTER_ARGS.replace(
            "c_CurrentD3D11On12InterfaceVersion = 7",
            "c_CurrentD3D11On12InterfaceVersion = 8")
        self.assertNotEqual(bumped, DRIVER_ADAPTER_ARGS)
        errors = check_adapter_args.check(bumped, CORE_ADAPTER_ARGS)
        self.assertTrue(any("InterfaceVersion" in error for error in errors),
                        errors)

    def test_a_driver_bump_that_removes_the_struct_is_rejected(self):
        """Silence is not agreement: if the gate cannot find what it compares,
        it must say so rather than pass."""
        gone = DRIVER_ADAPTER_ARGS.replace("struct SOpenAdapterArgs",
                                           "struct SOpenAdapterArgsRenamed")
        errors = check_adapter_args.check(gone, CORE_ADAPTER_ARGS)
        self.assertTrue(any("not found in the pinned driver" in error
                            for error in errors), errors)


class DtlPortabilityInventory(unittest.TestCase):
    def test_comments_do_not_inflate_the_inventory(self):
        source = "CComPtr<IUnknown> live; // CComPtr<IUnknown> old\n/* _com_error */"
        stripped = inventory_dtl_portability.without_comments(source)
        self.assertEqual(
            len(inventory_dtl_portability.PATTERNS["atl_com_ptr"].findall(stripped)),
            1)
        self.assertNotIn("_com_error", stripped)

    def test_inventory_records_occurrences_and_locations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "include").mkdir()
            (root / "include" / "probe.hpp").write_text(
                "#include <atlbase.h>\nCComPtr<IUnknown> first, second;")
            result = inventory_dtl_portability.inventory(root, revision="probe")
        self.assertEqual(result["revision"], "probe")
        self.assertEqual(result["categories"]["atl_headers"]["occurrences"], 1)
        self.assertEqual(result["categories"]["atl_com_ptr"]["files"],
                         {"include/probe.hpp": 1})

    def test_new_blocker_changes_the_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            path = root / "src" / "probe.cpp"
            path.write_text("int portable;")
            before = inventory_dtl_portability.inventory(root)
            path.write_text("throw _com_error(E_FAIL);")
            after = inventory_dtl_portability.inventory(root)
        self.assertNotEqual(before, after)
        self.assertEqual(after["categories"]["com_error"]["occurrences"], 1)

    def test_the_sdk_etw_header_and_the_event_sites_are_counted_apart(self):
        """The split is the point: one number could not tell "does not
        compile" from "compiles and does nothing"."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            (root / "src" / "probe.cpp").write_text(
                '#include <traceloggingprovider.h>\n'
                "if (g_hTracelogging) TraceLoggingWrite(g_hTracelogging, \"E\");")
            result = inventory_dtl_portability.inventory(root)
        categories = result["categories"]
        self.assertEqual(
            categories["tracelogging_sdk_headers"]["occurrences"], 1)
        # g_hTracelogging twice and TraceLoggingWrite once.
        self.assertEqual(categories["tracelogging_events"]["occurrences"], 3)

    def test_removing_the_sdk_etw_header_leaves_the_events_counted(self):
        """Replacing the header must not silently zero the retained sites."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "src").mkdir()
            path = root / "src" / "probe.cpp"
            path.write_text('#include <traceloggingprovider.h>\n'
                            "TraceLoggingWrite(g_hTracelogging, \"E\");")
            before = inventory_dtl_portability.inventory(root)
            path.write_text('#include "relay_tracelogging.hpp"\n'
                            "TraceLoggingWrite(g_hTracelogging, \"E\");")
            after = inventory_dtl_portability.inventory(root)
        self.assertEqual(
            before["categories"]["tracelogging_sdk_headers"]["occurrences"], 1)
        self.assertEqual(
            after["categories"]["tracelogging_sdk_headers"]["occurrences"], 0)
        self.assertEqual(
            after["categories"]["tracelogging_events"]["occurrences"],
            before["categories"]["tracelogging_events"]["occurrences"])

    def test_relay_compat_headers_do_not_count_as_upstream_debt(self):
        """A shim must not inflate the category it exists to empty."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "include").mkdir()
            (root / "include" / "relay_tracelogging.hpp").write_text(
                "using TraceLoggingHProvider = void*;\n"
                "#define TraceLoggingWrite(...) ((void)0)")
            (root / "include" / "relay_atl_compat.hpp").write_text(
                "template <typename T> class CComPtr;")
            result = inventory_dtl_portability.inventory(root)
        self.assertEqual(
            result["categories"]["tracelogging_events"], {"files": {}, "occurrences": 0})
        self.assertEqual(
            result["categories"]["atl_com_ptr"], {"files": {}, "occurrences": 0})

    def test_the_exclusion_is_scoped_to_relay_headers_in_include(self):
        """Upstream files must still count even next to the shims, and a
        relay-prefixed file elsewhere is not an exemption."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "include").mkdir()
            (root / "src").mkdir()
            (root / "include" / "Upstream.hpp").write_text(
                "TraceLoggingWrite(g_hTracelogging, \"E\");")
            (root / "src" / "relay_not_a_shim.cpp").write_text(
                "TraceLoggingWrite(g_hTracelogging, \"E\");")
            result = inventory_dtl_portability.inventory(root)
        files = result["categories"]["tracelogging_events"]["files"]
        self.assertEqual(files,
                         {"include/Upstream.hpp": 2,
                          "src/relay_not_a_shim.cpp": 2})

    def test_dtl_baseline_has_no_sdk_etw_header(self):
        baseline = (REPOSITORY / "docs" /
                    "dtl-portability-baseline.json").read_text()
        self.assertEqual(
            json.loads(baseline)["categories"]["tracelogging_sdk_headers"],
            {"files": {}, "occurrences": 0})

    def test_dtl_baseline_has_no_msvc_language_extensions(self):
        baseline = (REPOSITORY / "docs" /
                    "dtl-portability-baseline.json").read_text()
        categories = json.loads(baseline)["categories"]
        self.assertEqual(categories["msvc_declspec"],
                         {"files": {}, "occurrences": 0})
        self.assertEqual(categories["msvc_uuidof"],
                         {"files": {}, "occurrences": 0})
        self.assertEqual(categories["cmake_msvc_linkage"],
                         {"files": {}, "occurrences": 0})

    def test_the_etw_shim_never_consumes_its_arguments(self):
        """The no-op macros must not name their parameters. If they did, the
        undefined field macros in the pinned call sites would start expanding
        and arguments with side effects would fire."""
        shim = (REPOSITORY / "compat" / "relay_tracelogging.hpp").read_text()
        for macro in ("TraceLoggingWrite", "TraceLoggingProviderEnabled"):
            definition = [line for line in shim.splitlines()
                          if line.startswith(f"#define {macro}(")]
            self.assertEqual(len(definition), 1, macro)
            self.assertIn("(...)", definition[0])
            self.assertNotIn("__VA_ARGS__", definition[0])

    def test_a_high_water_rise_without_acknowledgement_is_rejected(self):
        """The rule the old gate could not enforce: regenerating the baseline
        in the same commit made any increase pass."""
        baseline = {
            "schema": 1, "revision": "probe",
            "categories": {"widgets": {"occurrences": 5, "files": {}}},
            "high_water": {"widgets": 4},
        }
        errors = inventory_dtl_portability.check_high_water(baseline)
        self.assertTrue(any("exceeds the recorded high_water" in e
                            for e in errors), errors)

    def test_an_acknowledged_rise_is_allowed(self):
        baseline = {
            "schema": 1, "revision": "probe",
            "categories": {"widgets": {"occurrences": 5, "files": {}}},
            "high_water": {"widgets": 4},
            "acknowledged_increases": {"widgets": "retained by design"},
        }
        self.assertEqual(
            inventory_dtl_portability.check_high_water(baseline), [])

    def test_a_category_without_a_high_water_is_rejected(self):
        """A new category must not arrive unbounded."""
        baseline = {
            "schema": 1, "revision": "probe",
            "categories": {"widgets": {"occurrences": 1, "files": {}}},
            "high_water": {},
        }
        errors = inventory_dtl_portability.check_high_water(baseline)
        self.assertTrue(any("no high_water recorded" in e for e in errors),
                        errors)

    def test_the_committed_baseline_is_within_its_high_water(self):
        baseline = json.loads(
            (REPOSITORY / "docs" / "dtl-portability-baseline.json").read_text())
        self.assertEqual(
            inventory_dtl_portability.check_high_water(baseline), [])


class DtlStructReturnGate(unittest.TestCase):
    def test_a_direct_struct_returning_call_is_rejected(self):
        errors = check_dtl_struct_return.check_source(
            "src/bad.cpp", "auto luid = pDevice->GetAdapterLuid();")
        self.assertTrue(any("RelayD3D12AdapterLuid" in e for e in errors),
                        errors)

    def test_a_wrapped_call_passes(self):
        self.assertEqual(
            check_dtl_struct_return.check_source(
                "src/good.cpp", "auto luid = RelayD3D12AdapterLuid(pDevice);"),
            [])

    def test_comments_do_not_trip_the_gate(self):
        self.assertEqual(
            check_dtl_struct_return.check_source(
                "src/notes.cpp", "// pDevice->GetAdapterLuid() was replaced"),
            [])

    def test_the_trees_own_GetDesc_wrappers_are_not_flagged(self):
        """D3D12TranslationLayer declares its own GetDesc() on VideoDecode and
        friends. A textual rule cannot see types, so bare GetDesc is out of
        scope on purpose and this pins that choice."""
        self.assertEqual(
            check_dtl_struct_return.check_source(
                "src/video.cpp", "auto d = pVideoDecoder->GetDesc();"),
            [])

    def test_every_wrapped_method_is_detected(self):
        for method, wrapper in check_dtl_struct_return.WRAPPED_METHODS.items():
            errors = check_dtl_struct_return.check_source(
                "src/bad.cpp", f"x = p->{method}();")
            self.assertTrue(any(wrapper in e for e in errors), method)

    def test_dtl_baseline_has_no_executable_com_error(self):
        baseline = (REPOSITORY / "docs" /
                    "dtl-portability-baseline.json").read_text()
        self.assertEqual(
            json.loads(baseline)["categories"]["com_error"],
            {"files": {}, "occurrences": 0})

    def test_hresult_replacement_is_expression_for_expression(self):
        patch = (REPOSITORY / "patches" / "dtl" /
                 "0002-replace-msvc-com-error.patch").read_text()
        removed = [line[1:].replace("_com_error", "RelayHResultError")
                   for line in patch.splitlines()
                   if line.startswith("-") and "_com_error" in line]
        added = [line[1:] for line in patch.splitlines()
                 if line.startswith("+") and "RelayHResultError" in line]
        self.assertEqual(len(removed), 31)
        self.assertEqual(added, removed)


class DtlIncludeCaseGate(unittest.TestCase):
    def test_external_include_root_case_is_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory) / "consumer"
            dependency = pathlib.Path(directory) / "dependency"
            root.mkdir()
            dependency.mkdir()
            (root / "consumer.hpp").write_text("#include <dxbcutils.h>\n")
            (dependency / "DXBCUtils.h").write_text("#pragma once\n")
            errors = check_dtl_include_case.check_tree(root, [dependency])
            self.assertTrue(any("DXBCUtils.h" in error for error in errors))

    def test_matching_local_include_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "include").mkdir()
            (root / "include" / "DXBCUtils.h").write_text("#pragma once\n")
            (root / "probe.cpp").write_text('#include "DXBCUtils.h"\n')
            self.assertEqual(check_dtl_include_case.check_tree(root), [])

    def test_mismatch_reports_requested_and_actual_spelling(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "ShaderBinary.h").write_text("#pragma once\n")
            (root / "probe.cpp").write_text('#include <shaderbinary.h>\n')
            errors = check_dtl_include_case.check_tree(root)
            self.assertTrue(any(
                "'shaderbinary.h'" in error and "ShaderBinary.h" in error
                for error in errors), errors)

    def test_missing_external_header_is_out_of_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "probe.cpp").write_text("#include <windows.h>\n")
            self.assertEqual(check_dtl_include_case.check_tree(root), [])

    def test_duplicate_case_variants_are_all_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "one").mkdir()
            (root / "two").mkdir()
            (root / "one" / "Thing.h").write_text("#pragma once\n")
            (root / "two" / "THING.H").write_text("#pragma once\n")
            (root / "probe.cpp").write_text('#include "thing.h"\n')
            errors = check_dtl_include_case.check_tree(root)
            self.assertTrue(any(
                "THING.H, Thing.h" in error for error in errors), errors)

    def test_comments_are_not_includes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "Actual.h").write_text("#pragma once\n")
            (root / "probe.cpp").write_text(
                '// #include "actual.h"\n/*\n#include <ACTUAL.H>\n*/\n')
            self.assertEqual(check_dtl_include_case.check_tree(root), [])


class CleanroomIsolationGate(unittest.TestCase):
    def test_dependencies_outside_overlay_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            overlay = root / "overlay"
            overlay.mkdir()
            depfile = root / "clean.d"
            depfile.write_text("clean.o: tests/probe.c relay12-d3d11/ddi/probe.h\n")
            self.assertEqual(
                check_cleanroom_isolation.check(overlay, [depfile]), [])

    def test_overlay_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            overlay = root / "overlay"
            overlay.mkdir()
            proprietary = overlay / "d3d10umddi.h"
            proprietary.write_text("/* package input */\n")
            depfile = root / "leaked.d"
            depfile.write_text(f"leaked.o: tests/probe.c {proprietary}\n")
            errors = check_cleanroom_isolation.check(overlay, [depfile])
            self.assertTrue(any("d3d10umddi.h" in error for error in errors),
                            errors)

    def test_malformed_dependency_file_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            overlay = root / "overlay"
            overlay.mkdir()
            depfile = root / "broken.d"
            depfile.write_text("not a dependency record\n")
            errors = check_cleanroom_isolation.check(overlay, [depfile])
            self.assertTrue(any("malformed" in error for error in errors),
                            errors)


class DdiHeaderGate(unittest.TestCase):
    def test_a_well_formed_group_passes(self):
        self.assertEqual(check_ddi_header.check_provenance(GOOD_GROUP), [])

    def test_the_committed_headers_pass(self):
        self.assertEqual(
            check_ddi_header.check_provenance(DDI_HEADER.read_text()), [])
        self.assertEqual(
            check_ddi_header.check_no_pragma_pack(DDI_HEADER.read_text()), [])

    def test_the_rules_own_worked_example_is_not_a_group(self):
        """The headers state the required form by showing it, with /### for
        delimiters so the example is not a comment.  Its placeholders are not a
        URL or a date, and the gate must not demand that they be."""
        example = """\
/*
 * Rules:
 *
 *          /###
 *           * Group: <name>
 *           * Specification: <public URL>
 *           * Retrieved: <YYYY-MM-DD>
 *           ###/
 */
"""
        self.assertEqual(check_ddi_header.check_provenance(example), [])

    def test_a_real_group_cannot_hide_behind_the_example(self):
        """Structure decides, not the placeholder text: a group that opens a
        real comment is checked however much it resembles the example."""
        disguised = """\
/*
 * Group: <name>
 * Specification: <public URL>
 * Retrieved: <YYYY-MM-DD>
 */
"""
        errors = check_ddi_header.check_provenance(disguised)
        self.assertTrue(any("not a public URL" in error for error in errors))
        self.assertTrue(any("not a YYYY-MM-DD" in error for error in errors))

    def test_an_empty_citation_is_rejected(self):
        """The hole this gate was rewritten for: the old check tested only
        whether the string appeared, so a group citing nothing passed."""
        empty = GOOD_GROUP.replace(
            "Specification: https://learn.microsoft.com/en-us/example",
            "Specification:")
        errors = check_ddi_header.check_provenance(empty)
        self.assertTrue(any("empty" in error for error in errors), errors)

    def test_a_citation_that_is_not_a_url_is_rejected(self):
        errors = check_ddi_header.check_provenance(GOOD_GROUP.replace(
            "https://learn.microsoft.com/en-us/example", "the WDK header"))
        self.assertTrue(any("not a public URL" in error for error in errors))

    def test_a_malformed_date_is_rejected(self):
        for date in ("2026-13-45", "07/09/2026", "yesterday", ""):
            with self.subTest(date=date):
                errors = check_ddi_header.check_provenance(
                    GOOD_GROUP.replace("2026-09-07", date))
                self.assertTrue(errors, f"{date!r} was accepted as a date")

    def test_a_missing_citation_is_rejected(self):
        for line in ("Specification", "Retrieved"):
            with self.subTest(missing=line):
                stripped = "\n".join(
                    entry for entry in GOOD_GROUP.splitlines()
                    if f"* {line}:" not in entry)
                errors = check_ddi_header.check_provenance(stripped)
                self.assertTrue(errors, f"a group with no {line} passed")

    def test_an_unnamed_group_is_rejected(self):
        errors = check_ddi_header.check_provenance(
            GOOD_GROUP.replace("Group: a well formed group", "Group:"))
        self.assertTrue(any("needs a name" in error for error in errors))

    def test_provenance_may_sit_further_down_the_block(self):
        """The check this replaces inspected a four-line window, so a group
        with prose before its citation failed for no reason."""
        spaced = GOOD_GROUP.replace(
            " * Group: a well formed group\n",
            " * Group: a well formed group\n *\n * Four\n * lines\n * of\n"
            " * prose.\n *\n")
        self.assertEqual(check_ddi_header.check_provenance(spaced), [])

    def test_an_unclosed_block_is_rejected(self):
        errors = check_ddi_header.check_provenance(
            "/*\n * Group: unterminated\n * Specification: https://x/\n")
        self.assertTrue(any("not closed" in error for error in errors))

    def test_pragma_pack_is_rejected_but_naming_it_is_not(self):
        self.assertEqual(check_ddi_header.check_no_pragma_pack(
            " * #pragma pack is prohibited in this header.\n"), [])
        for pragma in ("#pragma pack(1)", "  #pragma  pack(push, 8)",
                       "#\tpragma pack()"):
            with self.subTest(pragma=pragma):
                self.assertTrue(
                    check_ddi_header.check_no_pragma_pack(pragma + "\n"))


class InterfaceAcquisitionGate(unittest.TestCase):
    def test_a_funnelled_acquisition_passes(self):
        source = ("hr = strictResult(device->QueryInterface(iid, out), "
                  "holder);\n")
        self.assertEqual(
            check_interface_acquisition.find_violations(source), [])

    def test_the_committed_sources_pass(self):
        for path in sorted((REPOSITORY / "relay12-d3d11").rglob("*.cpp")):
            with self.subTest(source=path.name):
                self.assertEqual(
                    check_interface_acquisition.find_violations(
                        path.read_text()), [])

    def test_a_bypassed_acquisition_is_rejected(self):
        for source in ("hr = device->QueryInterface(iid, out);\n",
                       "hr = queue->GetDevice(iid, out);\n",
                       "if (FAILED(queue->GetDevice (iid, out)))\n"):
            with self.subTest(source=source.strip()):
                self.assertEqual(
                    len(check_interface_acquisition.find_violations(source)), 1)


# objdump -p output, trimmed to the parts the audit reads.
OBJDUMP = """\
d3d11shim.dll:     file format pei-x86-64

The Export Tables (interpreted .edata section contents)

Export Flags                    0
Ordinal Base                    1
Number in:
\tExport Address Table           \t00000004
\t[Name Pointer/Ordinal] Table   \t00000004

[Ordinal/Name Pointer] Table
\t[   0] D3D11CreateDevice
\t[   1] D3D11CreateDeviceAndSwapChain
\t[   2] D3D11On12CreateDevice
\t[   3] WineD3D11ShimGetStatus

There is an import table in .idata

The Import Tables (interpreted .idata section contents)

\tDLL Name: KERNEL32.dll
\tvma:  Hint/Ord

\tDLL Name: msvcrt.dll
\tvma:  Hint/Ord
"""


class PeAudit(unittest.TestCase):
    def test_exports_are_read_with_the_ordinal_base(self):
        """The name-pointer table is indexed from zero and the base is printed
        separately, so reading the indices as ordinals would pass a module
        whose base was not 1."""
        self.assertEqual(check_pe_audit.parse_exports(OBJDUMP), {
            1: "D3D11CreateDevice",
            2: "D3D11CreateDeviceAndSwapChain",
            3: "D3D11On12CreateDevice",
            4: "WineD3D11ShimGetStatus",
        })
        rebased = OBJDUMP.replace("Ordinal Base                    1",
                                  "Ordinal Base                    7")
        self.assertEqual(min(check_pe_audit.parse_exports(rebased)), 7)

    def test_llvm_objdump_exports_are_read(self):
        """llvm-mingw's objdump is llvm-objdump, which prints a different
        export table. The toolchain migration to llvm-mingw silently turned
        this gate into a no-op once already; both formats must parse to the
        same table."""
        llvm_objdump = (
            "Export Table:\n"
            " DLL name: d3d11shim.dll\n"
            " Ordinal base: 1\n"
            " Ordinal      RVA  Name\n"
            "       1   0x1250  D3D11CreateDevice\n"
            "       2   0x1260  D3D11CreateDeviceAndSwapChain\n"
            "       3   0x1270  D3D11On12CreateDevice\n"
            "       4   0x1280  WineD3D11ShimGetStatus\n"
            "\n"
            "The Import Tables:\n"
            "    DLL Name: KERNEL32.dll\n"
        )
        self.assertEqual(check_pe_audit.parse_exports(llvm_objdump),
                         check_pe_audit.parse_exports(OBJDUMP))

    def test_unreadable_export_table_is_an_error_not_an_empty_table(self):
        """Returning {} for output it cannot read is what made the llvm-mingw
        migration silent: the caller compared an empty table and exited
        non-zero with nothing to read."""
        with self.assertRaises(ValueError):
            check_pe_audit.parse_exports("a third objdump's output\n")

    def test_imports_are_lowercased(self):
        self.assertEqual(check_pe_audit.parse_imports(OBJDUMP),
                         {"kernel32.dll", "msvcrt.dll"})

    def test_the_expected_router_passes(self):
        self.assertEqual(
            check_pe_audit.audit("d3d11shim.dll", OBJDUMP, False), [])

    def test_a_renamed_export_is_rejected(self):
        renamed = OBJDUMP.replace("D3D11On12CreateDevice",
                                  "D3D11On12CreateDeviceEx")
        errors = check_pe_audit.audit("d3d11shim.dll", renamed, False)
        self.assertTrue(any("exports" in error for error in errors), errors)

    def test_a_reordered_export_is_rejected(self):
        """Applications may bind by ordinal, so the order is the contract."""
        reordered = OBJDUMP.replace(
            "\t[   0] D3D11CreateDevice\n"
            "\t[   1] D3D11CreateDeviceAndSwapChain\n",
            "\t[   0] D3D11CreateDeviceAndSwapChain\n"
            "\t[   1] D3D11CreateDevice\n")
        self.assertTrue(check_pe_audit.audit("d3d11shim.dll", reordered, False))

    def test_an_extra_import_is_rejected(self):
        extra = OBJDUMP.replace("\tDLL Name: msvcrt.dll",
                                "\tDLL Name: ntdll.dll\n\tvma:  Hint/Ord\n"
                                "\n\tDLL Name: msvcrt.dll")
        errors = check_pe_audit.audit("d3d11shim.dll", extra, False)
        self.assertTrue(any("imports" in error for error in errors), errors)

    def test_either_c_runtime_is_accepted(self):
        """llvm-mingw publishes an msvcrt and a ucrt build of every release,
        and the same snprintf/fputs calls link against msvcrt.dll under one
        and a set of api-ms-win-crt-* stubs under the other.  The pinned
        toolchain is the ucrt build, so that shape has to pass; the rule this
        gate enforces is that nothing beyond kernel32 and a C runtime appears
        either way."""
        ucrt = "\n".join(
            f"\tDLL Name: {name}\n\tvma:  Hint/Ord"
            for name in sorted(check_pe_audit.CRT_IMPORTS[1]))
        text = OBJDUMP.replace("\tDLL Name: msvcrt.dll\n\tvma:  Hint/Ord",
                               ucrt)
        self.assertEqual(
            check_pe_audit.parse_imports(text),
            {"kernel32.dll"} | check_pe_audit.CRT_IMPORTS[1])
        self.assertEqual(check_pe_audit.audit("d3d11shim.dll", text, False),
                         [])

    def test_a_graphics_import_is_rejected_under_either_runtime(self):
        """The router resolves every driver and diagnostic entry point at run
        time.  A real import of one means that stopped happening, and it must
        not be excused by whichever C runtime is in use."""
        for index, crt in enumerate(check_pe_audit.CRT_IMPORTS):
            with self.subTest(crt=sorted(crt)):
                imports = "\n".join(
                    f"\tDLL Name: {name}\n\tvma:  Hint/Ord"
                    for name in sorted(crt | {"d3d12.dll"}))
                text = OBJDUMP.replace(
                    "\tDLL Name: msvcrt.dll\n\tvma:  Hint/Ord", imports)
                errors = check_pe_audit.audit("d3d11shim.dll", text, False)
                self.assertTrue(any("imports" in error for error in errors),
                                (index, errors))

    def test_a_cxx_runtime_dependency_is_rejected(self):
        for library in ("libstdc++-6.dll", "libgcc_s_seh-1.dll"):
            with self.subTest(library=library):
                text = OBJDUMP + f"\n\tDLL Name: {library}\n"
                self.assertTrue(check_pe_audit.find_cxx_runtime(text))
                self.assertTrue(
                    check_pe_audit.audit("anything.exe", text, True))

    def test_an_unknown_module_needs_an_expectation(self):
        errors = check_pe_audit.audit("d3d11mystery.dll", OBJDUMP, False)
        self.assertTrue(any("no expected export table" in error
                            for error in errors), errors)

    def test_the_expected_tables_match_the_def_files(self):
        """EXPECTED_EXPORTS is transcribed from the .def files rather than
        parsed from them, so that a .def reorder fails instead of being
        adopted.  That only holds while the transcription is current: this
        gate's table sat at four entries while the core's .def had grown to
        eighteen, and the workflow's own inline copy was what caught the
        exports until it broke on a toolchain change.  This is the check that
        the two agree."""
        for module, source in (
            ("d3d11shim.dll", "d3d11shim.def"),
            ("d3d11on12core.dll", "d3d11on12core.def"),
        ):
            with self.subTest(module=module):
                text = (REPOSITORY / "relay12-d3d11" / source).read_text()
                declared = {}
                for line in text.splitlines():
                    line = line.split(";", 1)[0].strip()
                    if not line or line == "EXPORTS":
                        continue
                    # "NAME @N" or "NAME = internal_name @N"
                    match = re.match(r"(\S+)(?:\s*=\s*\S+)?\s+@(\d+)$", line)
                    self.assertIsNotNone(
                        match, f"{source}: unparsed export line {line!r}")
                    declared[int(match.group(2))] = match.group(1)
                self.assertEqual(
                    check_pe_audit.EXPECTED_EXPORTS[module], declared,
                    f"check_pe_audit.EXPECTED_EXPORTS[{module!r}] and "
                    f"relay12-d3d11/{source} disagree")


class LayoutModel(unittest.TestCase):
    """gen_ddi_layout.py --check is the second opinion on the header's
    offsets.  Its three fault classes were verified by hand once; this is so
    they stay verified."""

    def setUp(self):
        self.header = DDI_HEADER.read_text()

    def check(self, text):
        return gen_ddi_layout.check(written(text))

    def test_the_committed_header_agrees_with_the_model(self):
        self.assertEqual(self.check(self.header), [])

    def test_a_wrong_offset_is_caught(self):
        broken = self.header.replace(
            "WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, hRTCoreLayer, 56)",
            "WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, hRTCoreLayer, 48)")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(any("the model computes 56" in error
                            for error in errors), errors)

    def test_an_unasserted_field_is_caught(self):
        broken = self.header.replace(
            "WINE_DDI_ASSERT_FIELD(DXGI_DDI_BASE_ARGS, pDXGIBaseCallbacks, 0);\n",
            "")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(any("not asserted anywhere" in error
                            for error in errors), errors)

    def test_an_assertion_for_an_unmodelled_field_is_caught(self):
        broken = self.header.replace(
            "WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, Flags, 72);",
            "WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, Flags, 72);\n"
            "WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, "
            "pWDDM2_2UMCallbacks, 64);")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(any("absent from the model" in error
                            for error in errors), errors)

    def test_a_wrong_size_is_caught(self):
        broken = self.header.replace(
            "WINE_DDI_ASSERT_SIZE(D3D10DDIARG_CREATEDEVICE, 88)",
            "WINE_DDI_ASSERT_SIZE(D3D10DDIARG_CREATEDEVICE, 96)")
        self.assertNotEqual(broken, self.header)
        self.assertTrue(any("88 bytes" in error for error in self.check(broken)))

    def test_the_model_derives_the_padding_it_expects_to_be_named(self):
        """The gate is worthless if the model finds no gaps to name, so pin
        the set.  Every other group is all-pointer or all-4-byte and pads
        nowhere; if one of these stops padding, or a new one starts, that is a
        layout change and this is where it surfaces."""
        padding = {
            struct.name: struct.padding()
            for struct in gen_ddi_layout.GROUPS
            if struct.padding()
        }
        self.assertEqual(padding, {
            "D3D11_1_DDI_RENDER_TARGET_BLEND_DESC": [(37, 3)],
            "D3D10_DDI_DEPTH_STENCIL_DESC": [(26, 2)],
            "D3D10DDIARG_CREATEELEMENTLAYOUT": [(12, 4)],
            "D3D10DDIARG_CREATEDEVICE": [(76, 4)],
            "D3D11DDIARG_CREATEDEFERREDCONTEXT": [(36, 4)],
            "D3D11DDIARG_CREATERESOURCE": [(76, 4)],
            "D3D11DDI_HANDLESIZE": [(4, 4)],
            "D3D10DDIARG_OPENRESOURCE": [(4, 4), (20, 4), (36, 4)],
            "D3DDDICB_ESCAPE": [(12, 4), (28, 4)],
            "D3DDDICB_SYNCTOKEN": [(12, 4)],
            "D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW": [(28, 4)],
        })

    def test_pipeline_descriptor_emission_preserves_arrays_and_byte_padding(self):
        for model in (gen_ddi_layout.PSO_RT_BLEND, gen_ddi_layout.PSO_BLEND,
                      gen_ddi_layout.PSO_STENCIL_OP, gen_ddi_layout.PSO_DEPTH,
                      gen_ddi_layout.PSO_RASTER):
            with self.subTest(structure=model.name):
                self.assertIn("\n".join(gen_ddi_layout.emit(model)), self.header)

    def test_unasserted_padding_is_caught(self):
        """This model reads the header's assertions, not its declarations, so
        what it catches is a gap nothing asserts.  A pad member deleted from
        the declaration while its assertion stays would not compile, and the
        -Wpadded gate in tests/d3d11ddipadding.c is what catches one deleted
        from both."""
        broken = self.header.replace(
            "WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, WinePad1, 28);\n", "")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(any("padding at 28 that no member names" in error
                            for error in errors), errors)

    def test_a_pad_asserted_where_nothing_pads_is_caught(self):
        broken = self.header.replace(
            "WINE_DDI_ASSERT_FIELD(D3DDDICB_SYNCTOKEN, WinePad0, 12);",
            "WINE_DDI_ASSERT_FIELD(D3DDDICB_SYNCTOKEN, WinePad0, 16);")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(any("where the model derives no padding" in error
                            for error in errors), errors)

    def test_pads_numbered_out_of_offset_order_are_caught(self):
        broken = self.header.replace(
            "WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, WinePad0, 12);",
            "WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, WinePad2, 12);")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(any("numbered in offset order" in error
                            for error in errors), errors)

    def test_every_promoted_slot_is_authored(self):
        """Promotion is the one change to the device table no layout
        assertion can see: a promoted pointer and a placeholder pointer are
        both eight bytes at the same offset."""
        self.assertTrue(gen_ddi_layout.PROMOTED_SLOTS)
        for type_name in gen_ddi_layout.PROMOTED_SLOTS:
            with self.subTest(slot=type_name):
                self.assertNotIn(
                    f"typedef PFNWINE_D3D11DDI_UNDECLARED_CB {type_name};",
                    self.header)
                self.assertIn(f"(*{type_name})(", self.header)

    def test_a_promoted_slot_regressed_to_a_placeholder_is_caught(self):
        broken = self.header.replace(
            "typedef VOID (*PFND3D11DDI_RECYCLECOMMANDLIST)(\n"
            "        D3D10DDI_HDEVICE hDevice,\n"
            "        D3D11DDI_HCOMMANDLIST hCommandList);",
            "typedef PFNWINE_D3D11DDI_UNDECLARED_CB "
            "PFND3D11DDI_RECYCLECOMMANDLIST;")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(any("still placeholder aliases" in error
                            for error in errors), errors)

    def test_an_unrecorded_promotion_is_caught(self):
        # Any slot that is still a placeholder will do. When it is
        # promoted, repoint this at another placeholder rather than deleting
        # it -- assertNotEqual below is what stops the substitution silently
        # becoming a no-op and the gate going untested.
        broken = self.header.replace(
            "typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DRAWAUTO;",
            "typedef VOID (*PFND3D10DDI_DRAWAUTO)("
            "D3D10DDI_HDEVICE hDevice);")
        self.assertNotEqual(broken, self.header)
        errors = self.check(broken)
        self.assertTrue(
            any("PFND3D10DDI_DRAWAUTO" in error for error in errors),
            errors)

    def test_the_declared_and_published_arms_agree(self):
        """Dropping the union arms the driver does not read must not move
        anything; this is what makes that a declaration choice."""
        for struct in gen_ddi_layout.GROUPS:
            with self.subTest(struct=struct.name):
                declared = struct.walk(published=False)
                published = struct.walk(published=True)
                self.assertEqual(declared[1], published[1])
                self.assertEqual(declared[2], published[2])
                for name, offset in declared[0].items():
                    if name in published[0]:
                        self.assertEqual(offset, published[0][name])


class SharedStateGate(unittest.TestCase):
    """Every rule the shared-state gate claims, given something that breaks
    it.  The gate reads C++ without a C++ parser, so the first thing to pin is
    that it finds the functions at all: anchored on the wrong brace style it
    would report success having inspected nothing."""

    SHIM = REPOSITORY / "relay12-d3d11" / "d3d11shim.cpp"
    DIAG = REPOSITORY / "relay12-d3d11" / "wine_d3d11_diag.cpp"
    CORE = REPOSITORY / "relay12-d3d11" / "d3d11on12core.cpp"

    def setUp(self):
        self.shim = self.SHIM.read_text()
        self.diag = self.DIAG.read_text()
        self.core = self.CORE.read_text()

    def check(self, text):
        return check_shared_state.check_source("probe.cpp", text)

    def test_the_committed_sources_pass(self):
        for path in (self.SHIM, self.DIAG, self.CORE):
            with self.subTest(source=path.name):
                self.assertEqual(
                    check_shared_state.check_source(path, path.read_text()), [])

    def test_the_gate_finds_the_functions_it_inspects(self):
        self.assertEqual(
            sorted(check_shared_state.function_bodies(self.diag)),
            ["resolveSinks", "wineD3D11DiagReport", "wineD3D11DiagReportOnce"])
        self.assertIn("initialize",
                      check_shared_state.function_bodies(self.shim))

    def test_forward_type_declarations_are_not_shared_state(self):
        source = """namespace Example
{
struct StructTag;
class ClassTag;
union UnionTag;
enum EnumTag;
}
"""
        self.assertEqual(
            check_shared_state.namespace_scope_definitions(source), [])
        self.assertEqual(self.check(source), [])

    def test_the_gate_follows_a_wrapped_InitOnceExecuteOnce(self):
        """The router wraps the call in initialize() and every entry point
        calls that, so a gate looking only for the literal call would reject
        the committed tree."""
        lines = self.shim.splitlines()
        bodies = check_shared_state.function_bodies(self.shim)
        initializers = check_shared_state.initializers_for(
            "initOnce", lines, bodies)
        self.assertIn("initialize", initializers)
        self.assertIn("shimD3D11On12CreateDevice", initializers)

    def test_a_latch_assigned_directly_is_rejected(self):
        broken = self.core.replace(
            "    return DXGI_ERROR_UNSUPPORTED;",
            "    reportedNoHost = 1;\n    return DXGI_ERROR_UNSUPPORTED;")
        self.assertNotEqual(broken, self.core)
        self.assertTrue(any("must move through an Interlocked* call" in error
                            for error in self.check(broken)))

    def test_a_latch_stepped_directly_is_rejected(self):
        broken = self.core.replace(
            "    return DXGI_ERROR_UNSUPPORTED;",
            "    ++reportedNoHost;\n    return DXGI_ERROR_UNSUPPORTED;")
        self.assertNotEqual(broken, self.core)
        self.assertTrue(any("must move through an Interlocked* call" in error
                            for error in self.check(broken)))

    def test_unannotated_shared_state_is_rejected(self):
        broken = self.diag.replace(
            "/* shared-state: published once through sinkOnce */\n", "")
        self.assertNotEqual(broken, self.diag)
        self.assertTrue(any("is shared mutable state at namespace scope"
                            in error for error in self.check(broken)))

    def test_an_annotation_naming_no_INIT_ONCE_is_rejected(self):
        broken = self.diag.replace("published once through sinkOnce",
                                   "published once through notAThing")
        self.assertNotEqual(broken, self.diag)
        self.assertTrue(any("is not an INIT_ONCE in this file" in error
                            for error in self.check(broken)))

    def test_a_touch_before_the_barrier_is_rejected(self):
        broken = self.shim.replace(
            "    initialize();\n"
            "    if (!backend.createDevice)",
            "    if (!backend.createDevice)\n"
            "        initialize();\n"
            "    if (!backend.createDevice)")
        self.assertNotEqual(broken, self.shim)
        self.assertTrue(any("before it executes 'initOnce'" in error
                            for error in self.check(broken)))

    def test_a_touch_with_no_barrier_is_rejected(self):
        broken = self.shim.replace(
            "    initialize();\n    if (!backend.on12Interface.createDevice)",
            "    if (!backend.on12Interface.createDevice)")
        self.assertNotEqual(broken, self.shim)
        self.assertTrue(any("without executing 'initOnce' first" in error
                            for error in self.check(broken)))


class GateEntryPoints(unittest.TestCase):
    """Each gate must also work as CI invokes it: from the repository root,
    with an exit status."""

    def run_gate(self, *arguments):
        return subprocess.run(
            [sys.executable, *arguments], cwd=REPOSITORY,
            capture_output=True, text=True)

    def test_the_gates_pass_on_the_committed_tree(self):
        for gate in (["scripts/check_ddi_header.py"],
                     ["scripts/check_interface_acquisition.py"],
                     ["scripts/check_shared_state.py"],
                     ["scripts/check_core_callbacks.py"],
                     ["scripts/gen_ddi_layout.py", "--check"]):
            with self.subTest(gate=gate[0]):
                result = self.run_gate(*gate)
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)

    def test_a_failing_gate_exits_nonzero(self):
        broken = written("/*\n * Group:\n */\n")
        result = self.run_gate("scripts/check_ddi_header.py", str(broken))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("needs a name", result.stderr)



    def test_check_secure_code(self):
        proc = subprocess.run(
            ["python3", "scripts/check_secure_code.py", "relay12-d3d11"],
            capture_output=True, text=True,
            cwd=os.path.dirname(os.path.dirname(__file__))
        )
        self.assertEqual(0, proc.returncode, "check_secure_code rejected relay12-d3d11")

        proc = subprocess.run(
            ["python3", "scripts/check_secure_code.py", "tests"],
            capture_output=True, text=True,
            cwd=os.path.dirname(os.path.dirname(__file__))
        )
        self.assertEqual(0, proc.returncode, "check_secure_code rejected tests")


class CoreCallbacksGate(unittest.TestCase):
    """The core-layer callback table handed to the driver must be complete.

    pfnPerformAmortizedProcessingCb is the entry that bites: D3D11On12's
    Device::PostSubmit dereferences it on every batch flush, so leaving it
    null crashes inside the driver rather than failing anything here.  The
    table is zeroed at allocation, so the omission compiles and links.

    Every rule the gate claims is given something that breaks it, and each
    mutation is checked to have actually changed the source -- a `replace`
    that quietly matched nothing would otherwise leave these passing against
    an unmodified core.
    """

    CORE = REPOSITORY / "relay12-d3d11" / "d3d11on12core.cpp"

    def setUp(self):
        self.header = DDI_HEADER.read_text(encoding="utf-8")
        self.core = self.CORE.read_text(encoding="utf-8")

    def check(self, core=None, header=None):
        return check_core_callbacks.check_core_callbacks(
            self.header if header is None else header,
            self.core if core is None else core)

    def mutated(self, pattern, replacement, count=0):
        """The core with a regex substitution that is asserted to have bitten."""
        broken = re.sub(pattern, replacement, self.core, count=count)
        self.assertNotEqual(broken, self.core,
                            f"the mutation {pattern!r} matched nothing")
        return broken

    def test_the_committed_core_passes(self):
        self.assertEqual(self.check(), [])

    def test_every_mandatory_callback_carries_a_reason(self):
        """The list is the gate's own documentation; an entry without a
        rationale is an entry the next reader will delete."""
        for name, why in check_core_callbacks.MANDATORY.items():
            with self.subTest(callback=name):
                self.assertTrue(why.strip(), name)

    def test_an_unassigned_callback_is_rejected(self):
        for name in check_core_callbacks.MANDATORY:
            with self.subTest(callback=name):
                broken = self.mutated(
                    rf"\n *state->coreCallbacks\.{name} = \w+;", "")
                self.assertTrue(
                    any(f"{name} is never assigned" in error
                        for error in self.check(broken)),
                    self.check(broken))

    def test_a_null_callback_is_rejected(self):
        for name in check_core_callbacks.MANDATORY:
            with self.subTest(callback=name):
                broken = self.mutated(
                    rf"(state->coreCallbacks\.{name} = )\w+;",
                    r"\1nullptr;")
                self.assertTrue(
                    any(f"{name} is assigned nullptr" in error
                        for error in self.check(broken)),
                    self.check(broken))

    def test_a_callback_with_no_definition_is_rejected(self):
        broken = self.mutated(
            r"(state->coreCallbacks\.pfnPerformAmortizedProcessingCb = )\w+;",
            r"\1hostPerformAmortizedProcessingTypo;")
        self.assertTrue(
            any("does not define as a CALLBACK" in error
                for error in self.check(broken)),
            self.check(broken))

    def test_a_callback_assigned_after_create_device_is_rejected(self):
        """CreateDevice is when the driver takes the table's address."""
        assignment = ("    state->coreCallbacks.pfnPerformAmortizedProcessingCb"
                      " = hostPerformAmortizedProcessing;\n")
        self.assertIn(assignment, self.core)
        broken = self.core.replace(assignment, "")
        landed = '    traceCreation("leave driver CreateDevice", hr);\n'
        self.assertIn(landed, broken)
        broken = broken.replace(landed, landed + assignment)
        self.assertTrue(
            any("after the driver's CreateDevice" in error
                for error in self.check(broken)),
            self.check(broken))

    def test_a_mandatory_name_the_header_lost_is_rejected(self):
        """The list must not go on checking a field that was renamed upstream:
        that is the silent pass every gate here exists to prevent."""
        header = self.header.replace(
            "PFND3D11DDI_PERFORM_AMORTIZED_PROCESSING_CB pfnPerformAmortizedProcessingCb;",
            "PFND3D11DDI_PERFORM_AMORTIZED_PROCESSING_CB pfnRenamedUpstreamCb;")
        self.assertNotEqual(header, self.header)
        self.assertTrue(
            any("has drifted from the header" in error
                for error in self.check(header=header)),
            self.check(header=header))


if __name__ == "__main__":
    unittest.main()
