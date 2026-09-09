/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Concurrency stress for the D3D11On12 core boundary.
 *
 * What this tests, and what it deliberately does not.
 *
 * docs/DDI-CONCURRENCY-TESTING.md asks for a suite that hammers the
 * D3DWDDM2_6DDI_DEVICEFUNCS router from many threads.  That is not what this
 * is, because it cannot be yet: every one of that table's 178 slots holds
 * PFNWINE_D3D11DDI_UNDECLARED_CB, so a test calling them would be racing stubs
 * it wrote itself and would pass no matter what the eventual implementation
 * does.  A test that cannot fail for the right reason is worse than no test,
 * because the roadmap then reads as though the ground were covered.
 *
 * So this hammers the code that does hold shared mutable state today:
 *
 *   - the three exported core entry points, called concurrently against one
 *     set of mock COM objects;
 *   - the seven one-shot diagnostic latches in d3d11on12core.cpp, which exist
 *     to stop an application that probes creation in a loop from flooding the
 *     log, and which are the reason the storm has to be concurrent rather than
 *     merely repeated;
 *   - wineD3D11DiagReportOnce itself, whose InterlockedCompareExchange is the
 *     only synchronization primitive in the two modules.
 *
 * Nothing here observes the diagnostics.  They reach Wine's internal sink, the
 * debugger channel, or stderr depending on how the process was started, and a
 * test that picked one would be testing the launcher.  The flood check belongs
 * to the CI step, which captures the process's stderr and requires each
 * diagnostic exactly once across every thread and iteration; this file asserts
 * the results and the reference counts, which it can do exactly.
 *
 * The join is bounded.  A lock-order bug here would otherwise hang the runner
 * until its own timeout, hours later and with no output; a watchdog turns that
 * into a failure with a name.
 *
 * Build: x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
 *            -Irelay12-d3d11 -c -o ddi_thread_stress.o \
 *            tests/ddi_thread_stress.c
 */
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

#include "d3d11on12core.h"
#include "wine_d3d11_diag.h"
/* The mock COM objects, shared with tests/d3d11on12coretest.c. */
#include "d3d11on12mocks.h"

/* Twelve threads, in the 8-to-16 band docs/DDI-CONCURRENCY-TESTING.md asks
 * for, and more than any CI runner has cores: oversubscription is what makes
 * the scheduler preempt inside the boundary rather than between calls. */
#define STRESS_THREADS 12
#define STRESS_ITERATIONS 400

/* Generous against a slow shared runner, short against a deadlock: a storm
 * that has not finished in a minute is not slow, it is stuck. */
#define STRESS_JOIN_TIMEOUT_MS 60000

static volatile LONG failures;

/* Called from every thread, so it cannot use the serial helpers the other
 * suites use.  Printing under a lock keeps two failures from interleaving into
 * one unreadable line, and the count is atomic because it is the value main
 * returns on. */
static CRITICAL_SECTION report_lock;

static void fail(const char *what, const char *detail)
{
    EnterCriticalSection(&report_lock);
    printf("[fail] %s: %s\n", what, detail);
    fflush(stdout);
    LeaveCriticalSection(&report_lock);
    InterlockedIncrement(&failures);
}

static void fail_hr(const char *what, HRESULT got, HRESULT expected)
{
    EnterCriticalSection(&report_lock);
    printf("[fail] %s: got 0x%08lx, expected 0x%08lx\n", what,
            (unsigned long)got, (unsigned long)expected);
    fflush(stdout);
    LeaveCriticalSection(&report_lock);
    InterlockedIncrement(&failures);
}

static void expect_hr(const char *what, HRESULT got, HRESULT expected)
{
    if (got != expected)
        fail_hr(what, got, expected);
}

/*
 * The objects every thread shares.
 *
 * Sharing them is the whole point.  Each thread with its own device and queue
 * would exercise the boundary concurrently but touch no common state, and the
 * reference counts -- which the core raises and drops on every call -- are the
 * common state most likely to tear.  mock_device and friends maintain them
 * with InterlockedIncrement and InterlockedDecrement, and this is what makes
 * that load-bearing rather than incidental.
 */
static struct mock_device shared_device;
static struct mock_queue shared_queue;
static struct mock_queue shared_copy_queue;
static struct mock_queue shared_orphan_queue;
static struct mock_queue shared_lying_queue;
static struct mock_unknown shared_not_d3d12;
static struct mock_unknown shared_liar;
static struct mock_device shared_nodeless_device;
/* Two devices that will not identify themselves, and a queue owned by the
 * second.  This is the only shape that reaches the identity latch: the
 * boundary needs a real ID3D12Device from both sides and needs them to differ
 * before it asks IUnknown at all. */
static struct mock_device shared_unidentifiable_device;
static struct mock_device shared_other_unidentifiable_device;
static struct mock_queue shared_foreign_queue;

/* The guard the report-once test owns.  The core's own seven latches are in an
 * anonymous namespace and cannot be reached from here; this one is reached the
 * same way the core reaches its own. */
static volatile LONG stress_report_guard;

static HRESULT create_with(IUnknown *device_object, UINT flags,
        IUnknown *const *queues, UINT queue_count, UINT node_mask)
{
    ID3D11Device *device11 = (ID3D11Device *)(ULONG_PTR)0xdeadbeefdeadbeefull;
    ID3D11DeviceContext *context11 =
            (ID3D11DeviceContext *)(ULONG_PTR)0xfeedfacefeedfaceull;
    D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0x9999;
    HRESULT hr;

    hr = WineD3D11On12CreateDeviceV1(device_object, flags, NULL, 0, queues,
            queue_count, node_mask, &device11, &context11, &level);

    /* Every call on every thread, not once at the end: an output the core
     * cleared on one thread and left poisoned on another is the failure this
     * suite exists to find, and checking after the storm would miss it. */
    if (device11 || context11 || level)
        fail("create-device outputs", "an output was not cleared");
    return hr;
}

static DWORD WINAPI stress_thread(void *parameter)
{
    IUnknown *good_queue[1] = { (IUnknown *)&shared_queue.ID3D12CommandQueue_iface };
    IUnknown *copy_queue[1] = { (IUnknown *)&shared_copy_queue.ID3D12CommandQueue_iface };
    IUnknown *orphan_queue[1] = { (IUnknown *)&shared_orphan_queue.ID3D12CommandQueue_iface };
    IUnknown *lying_queue[1] = { (IUnknown *)&shared_lying_queue.ID3D12CommandQueue_iface };
    IUnknown *not_a_queue[1] = { &shared_not_d3d12.IUnknown_iface };
    IUnknown *foreign_queue[1] =
            { (IUnknown *)&shared_foreign_queue.ID3D12CommandQueue_iface };
    IUnknown *good_device = (IUnknown *)&shared_device.ID3D12Device_iface;
    unsigned int iteration;

    (void)parameter;

    for (iteration = 0; iteration < STRESS_ITERATIONS; ++iteration)
    {
        WineD3D11On12Interface iface;
        UINT version;

        /* Pure function, but it is the entry point a deployment probe calls
         * first and the one a loader race would catch. */
        version = WineD3D11On12GetABIVersion();
        if (version != WINE_D3D11ON12_ABI_VERSION)
            fail("GetABIVersion", "returned the wrong ABI version");

        /* The interface table is written into caller storage.  Every field is
         * checked on every call: a core that filled it from shared scratch
         * space would pass a single-threaded test and tear here. */
        memset(&iface, 0xcc, sizeof(iface));
        expect_hr("GetInterface", WineD3D11On12GetInterface(
                WINE_D3D11ON12_ABI_VERSION, sizeof(iface), &iface), S_OK);
        if (iface.size != sizeof(iface)
                || iface.version != WINE_D3D11ON12_ABI_VERSION
                || iface.capabilities != WINE_D3D11ON12_CAP_VALIDATION
                || iface.createDevice != WineD3D11On12CreateDeviceV1)
            fail("GetInterface", "the interface table was filled wrongly");

        /* Accepted all the way to the end, which is the path that reports
         * "no host implemented" and the one that exercises every acquisition
         * and release in the boundary. */
        expect_hr("accepted device and queue",
                create_with(good_device, 0, good_queue, 1, 0),
                DXGI_ERROR_UNSUPPORTED);

        /* One call per rejection path, reaching all seven diagnostic latches,
         * so each is contended by all twelve threads rather than won by
         * whichever happened to start first.  Two of these reach one latch:
         * an object that is not a D3D12 device and one that claims to be
         * without returning an interface are the same finding by different
         * routes, and both routes are worth driving. */
        expect_hr("flags validated but not translated",
                create_with(good_device, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                        good_queue, 1, 0),
                DXGI_ERROR_UNSUPPORTED);
        expect_hr("device is not a D3D12 device",
                create_with(&shared_not_d3d12.IUnknown_iface, 0, good_queue, 1,
                        0),
                E_NOINTERFACE);
        expect_hr("device reports success with no interface",
                create_with(&shared_liar.IUnknown_iface, 0, good_queue, 1, 0),
                E_NOINTERFACE);
        expect_hr("queue is not a D3D12 queue",
                create_with(good_device, 0, not_a_queue, 1, 0), E_NOINTERFACE);
        expect_hr("queue will not report its device",
                create_with(good_device, 0, orphan_queue, 1, 0), E_FAIL);
        expect_hr("queue reports its device with no interface",
                create_with(good_device, 0, lying_queue, 1, 0), E_NOINTERFACE);
        expect_hr("neither device will identify itself",
                create_with((IUnknown *)&shared_unidentifiable_device
                                .ID3D12Device_iface,
                        0, foreign_queue, 1, 0),
                E_NOINTERFACE);
        expect_hr("device reports no nodes",
                create_with((IUnknown *)&shared_nodeless_device
                                .ID3D12Device_iface,
                        0, good_queue, 1, 0x1),
                E_INVALIDARG);

        /* Rejected before any diagnostic: a queue of the wrong type. */
        expect_hr("queue is not a direct queue",
                create_with(good_device, 0, copy_queue, 1, 0), E_INVALIDARG);

        /* The report-once primitive, contended directly.  Twelve threads
         * racing one guard is the case its comment claims to handle. */
        wineD3D11DiagReportOnce(&stress_report_guard,
                "ddi_thread_stress: the report-once guard was won.\n");
    }

    return 0;
}

/* Every mock is created holding one reference and the storm hands none of them
 * out, so one is what each must be back to.  A core that leaked an AddRef on
 * some rejection path, or a mock whose count tore under twelve threads, shows
 * up here and nowhere else in this suite. */
static void check_refcount(const char *what, LONG got)
{
    if (got == 1)
    {
        printf("[ ok ] %s holds the caller's single reference\n", what);
        return;
    }
    EnterCriticalSection(&report_lock);
    printf("[fail] %s: refcount is %ld after the storm, expected 1\n", what,
            (long)got);
    LeaveCriticalSection(&report_lock);
    InterlockedIncrement(&failures);
}

int main(void)
{
    HANDLE threads[STRESS_THREADS];
    DWORD waited;
    unsigned int index;

    InitializeCriticalSection(&report_lock);

    mock_device_init(&shared_device);
    mock_queue_init(&shared_queue, &shared_device,
            D3D12_COMMAND_LIST_TYPE_DIRECT);
    mock_queue_init(&shared_copy_queue, &shared_device,
            D3D12_COMMAND_LIST_TYPE_COPY);
    /* No owning device, so GetDevice fails outright. */
    mock_queue_init(&shared_orphan_queue, NULL,
            D3D12_COMMAND_LIST_TYPE_DIRECT);
    /* Reports success from GetDevice with no interface, the way the payload
     * does.  Same contract violation as a rejected query, so the boundary owes
     * it the same answer. */
    mock_queue_init(&shared_lying_queue, &shared_device,
            D3D12_COMMAND_LIST_TYPE_DIRECT);
    shared_lying_queue.lie_about_device = 1;
    mock_unknown_init(&shared_not_d3d12);
    mock_liar_init(&shared_liar);
    mock_device_init(&shared_nodeless_device);
    shared_nodeless_device.node_count = 0;
    mock_device_init(&shared_unidentifiable_device);
    mock_device_init(&shared_other_unidentifiable_device);
    shared_unidentifiable_device.lie_about_identity = 1;
    shared_other_unidentifiable_device.lie_about_identity = 1;
    mock_queue_init(&shared_foreign_queue,
            &shared_other_unidentifiable_device,
            D3D12_COMMAND_LIST_TYPE_DIRECT);

    for (index = 0; index < STRESS_THREADS; ++index)
    {
        threads[index] = CreateThread(NULL, 0, stress_thread, NULL, 0, NULL);
        if (!threads[index])
        {
            printf("[fail] could not create stress thread %u\n", index);
            return 1;
        }
    }

    waited = WaitForMultipleObjects(STRESS_THREADS, threads, TRUE,
            STRESS_JOIN_TIMEOUT_MS);
    if (waited == WAIT_TIMEOUT)
    {
        /* Deliberately not killed and not joined: a hung storm is reported and
         * the process exits, because the alternative is a runner that sits
         * until its own timeout with nothing printed. */
        printf("[fail] the storm did not finish in %u ms; the boundary "
                "deadlocked\n", (unsigned int)STRESS_JOIN_TIMEOUT_MS);
        printf("RESULT: 1 DDI concurrency failure(s)\n");
        fflush(stdout);
        return 1;
    }
    if (waited == WAIT_FAILED)
    {
        printf("[fail] waiting for the stress threads failed: %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }

    printf("[ ok ] %d threads completed %d iterations each\n", STRESS_THREADS,
            STRESS_ITERATIONS);

    for (index = 0; index < STRESS_THREADS; ++index)
        CloseHandle(threads[index]);

    check_refcount("the shared device", shared_device.refcount);
    check_refcount("the shared direct queue", shared_queue.refcount);
    check_refcount("the shared copy queue", shared_copy_queue.refcount);
    check_refcount("the shared orphan queue", shared_orphan_queue.refcount);
    check_refcount("the shared lying queue", shared_lying_queue.refcount);
    check_refcount("the shared non-D3D12 object", shared_not_d3d12.refcount);
    check_refcount("the shared identity liar", shared_liar.refcount);
    check_refcount("the shared nodeless device",
            shared_nodeless_device.refcount);
    check_refcount("the first unidentifiable device",
            shared_unidentifiable_device.refcount);
    check_refcount("the second unidentifiable device",
            shared_other_unidentifiable_device.refcount);
    check_refcount("the shared foreign queue", shared_foreign_queue.refcount);

    /* Won once, by one thread, out of twelve threads times every iteration. */
    if (stress_report_guard == 1)
    {
        printf("[ ok ] the report-once guard was taken exactly once\n");
    }
    else
    {
        printf("[fail] the report-once guard is %ld after the storm, "
                "expected 1\n", (long)stress_report_guard);
        InterlockedIncrement(&failures);
    }

    DeleteCriticalSection(&report_lock);

    if (failures)
    {
        printf("RESULT: %ld DDI concurrency failure(s)\n", (long)failures);
        return 1;
    }
    printf("RESULT: the core boundary is safe under %d concurrent callers\n",
            STRESS_THREADS);
    return 0;
}
