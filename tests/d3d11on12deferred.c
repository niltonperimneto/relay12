/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Deferred contexts and command lists against the mock driver.
 *
 * What the core promises, each given the input that would break it:
 *
 *   * every malformed call is refused before the driver sees it, and a
 *     refused call leaves the caller's structure cleared;
 *   * a deferred context gets its own function table, never the immediate
 *     one, and draws recorded through it reach the driver only when a
 *     command list made from it is executed -- once per execution;
 *   * an error the driver reports while a context records is returned by
 *     CreateCommandList instead of a list, and the context starts clean;
 *   * destroyed lists keep their memory for RecycleCreateCommandList, up to
 *     the pool limit, and no further;
 *   * destruction is idempotent and using a destroyed handle is refused;
 *   * closing the device destroys every context and list still alive.
 */

#include <stdio.h>
#include <string.h>

#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"
#include "d3d11on12mocks.h"

#define MOCK_DEFERRED_FAIL 0xdeadu
/* The core's commandListPoolLimit. */
#define POOL_LIMIT 8

static int failures;

#define CHECK(cond, name) do { \
        if (cond) printf("[ ok ] %s\n", name); \
        else { printf("[fail] %s (line %d)\n", name, __LINE__); ++failures; } \
    } while (0)

typedef void (WINAPI *deferred_counts_fn)(LONG *, LONG *, LONG *, LONG *, LONG *,
        LONG *, LONG *, int *);
typedef void (WINAPI *execute_counts_fn)(LONG *, LONG *);
typedef void (WINAPI *fail_fn)(void);

static deferred_counts_fn deferred_counts;
static execute_counts_fn execute_counts;

struct counts
{
    LONG contexts_created, contexts_destroyed, abandoned;
    LONG lists_created, lists_recycled, lists_destroyed, lists_recycle_destroyed;
    int bad;
    LONG executes, executed_draws;
};

static struct counts read_counts(void)
{
    struct counts c;

    deferred_counts(&c.contexts_created, &c.contexts_destroyed, &c.abandoned,
            &c.lists_created, &c.lists_recycled, &c.lists_destroyed,
            &c.lists_recycle_destroyed, &c.bad);
    execute_counts(&c.executes, &c.executed_draws);
    return c;
}

static void record(WineD3D11On12DeferredContext *context, UINT draws)
{
    D3D10DDI_HDEVICE handle;
    UINT i;

    handle.pDrvPrivate = context->hDrvContext;
    for (i = 0; i < draws; ++i)
        context->contextFuncs->pfnDraw(handle, 3, 0);
}

static void init_context(WineD3D11On12DeferredContext *context)
{
    memset(context, 0, sizeof(*context));
    context->size = sizeof(*context);
}

static void init_list(WineD3D11On12CommandList *list)
{
    memset(list, 0, sizeof(*list));
    list->size = sizeof(*list);
}

int main(void)
{
    WineD3D11On12AdapterDevice out, other;
    struct mock_device device, other_device;
    struct mock_queue queue, other_queue;
    IUnknown *queues[1], *other_queues[1];
    WineD3D11On12DeferredContext context, second, bad_context;
    WineD3D11On12CommandList list, again, lists[POOL_LIMIT + 2];
    fail_fn fail_next_context, fail_next_list;
    struct counts before, after;
    HMODULE driver;
    HRESULT hr;
    UINT i;

    driver = LoadLibraryW(L"d3d11on12.dll");
    deferred_counts = driver ? (deferred_counts_fn)(void *)GetProcAddress(driver,
            "WineD3D11On12MockDriverGetDeferredCounts") : NULL;
    execute_counts = driver ? (execute_counts_fn)(void *)GetProcAddress(driver,
            "WineD3D11On12MockDriverGetExecuteCounts") : NULL;
    fail_next_context = driver ? (fail_fn)(void *)GetProcAddress(driver,
            "WineD3D11On12MockDriverFailNextDeferredContext") : NULL;
    fail_next_list = driver ? (fail_fn)(void *)GetProcAddress(driver,
            "WineD3D11On12MockDriverFailNextCommandList") : NULL;
    if (!deferred_counts || !execute_counts || !fail_next_context || !fail_next_list)
    {
        printf("[fail] the mock driver's deferred-context exports are missing\n");
        return 1;
    }

    mock_device_init(&device);
    device.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &out);
    CHECK(hr == S_OK && out.deviceFuncs, "the mock adapter opens");
    if (FAILED(hr))
        return 1;

    /* ---- refusals ---- */
    before = read_counts();
    CHECK(WineD3D11On12CreateDeferredContextV1(&out, 0, NULL) == E_INVALIDARG,
            "a null context structure is refused");
    init_context(&bad_context);
    bad_context.size = sizeof(bad_context) - 1;
    CHECK(WineD3D11On12CreateDeferredContextV1(&out, 0, &bad_context) == E_INVALIDARG,
            "a mis-sized context structure is refused");
    init_context(&bad_context);
    bad_context.hDrvContext = (void *)(UINT_PTR)0x1234;
    CHECK(WineD3D11On12CreateDeferredContextV1(&out, 1, &bad_context) == E_INVALIDARG
            && !bad_context.hDrvContext && !bad_context.contextFuncs,
            "context flags are refused and the structure is cleared");
    init_context(&bad_context);
    CHECK(WineD3D11On12CreateDeferredContextV1(NULL, 0, &bad_context) == E_INVALIDARG,
            "a context without a device is refused");
    init_list(&list);
    CHECK(WineD3D11On12CreateCommandListV1(&bad_context, &list) == E_INVALIDARG,
            "a list from a context that was never created is refused");
    CHECK(WineD3D11On12ExecuteCommandListV1(&out, &list) == E_INVALIDARG,
            "executing a list that was never created is refused");
    after = read_counts();
    CHECK(after.contexts_created == before.contexts_created
            && after.lists_created == before.lists_created && after.executes == before.executes,
            "no refused call reached the driver");

    /* ---- one context, one list ---- */
    init_context(&context);
    hr = WineD3D11On12CreateDeferredContextV1(&out, 0, &context);
    CHECK(hr == S_OK && context.contextFuncs && context.hDrvContext && context.runtimeState,
            "a deferred context is created");
    if (FAILED(hr))
        return 1;
    CHECK(context.contextFuncs != out.deviceFuncs
            && context.contextFuncs->pfnDraw != out.deviceFuncs->pfnDraw,
            "the context records through its own table, not the immediate one");

    before = read_counts();
    record(&context, 5);
    after = read_counts();
    CHECK(after.executed_draws == before.executed_draws,
            "recorded draws do not run before a list is executed");

    init_list(&list);
    hr = WineD3D11On12CreateCommandListV1(&context, &list);
    after = read_counts();
    CHECK(hr == S_OK && list.hDrvCommandList && list.runtimeState
            && after.lists_created == before.lists_created + 1,
            "the first list is made from fresh memory with CreateCommandList");

    before = read_counts();
    CHECK(WineD3D11On12ExecuteCommandListV1(&out, &list) == S_OK, "the list executes");
    CHECK(WineD3D11On12ExecuteCommandListV1(&out, &list) == S_OK, "the list executes again");
    after = read_counts();
    CHECK(after.executes == before.executes + 2
            && after.executed_draws == before.executed_draws + 10,
            "each execution runs every recorded draw exactly once");

    init_list(&again);
    CHECK(WineD3D11On12CreateCommandListV1(&context, &again) == S_OK,
            "a context that recorded nothing still finishes a list");
    before = read_counts();
    WineD3D11On12ExecuteCommandListV1(&out, &again);
    after = read_counts();
    CHECK(after.executed_draws == before.executed_draws,
            "finishing a list leaves the context empty");

    /* ---- recycling ---- */
    before = read_counts();
    CHECK(WineD3D11On12DestroyCommandListV1(&again) == S_OK
            && !again.hDrvCommandList && !again.runtimeState,
            "destroying a list clears the handle");
    CHECK(WineD3D11On12DestroyCommandListV1(&again) == S_OK,
            "destroying a list twice is a no-op");
    after = read_counts();
    CHECK(after.lists_recycle_destroyed == before.lists_recycle_destroyed + 1
            && after.lists_destroyed == before.lists_destroyed,
            "a destroyed list keeps its memory through RecycleDestroyCommandList, once");
    CHECK(WineD3D11On12ExecuteCommandListV1(&out, &again) == E_INVALIDARG,
            "a destroyed list is refused before the driver");

    before = read_counts();
    record(&context, 2);
    init_list(&again);
    hr = WineD3D11On12CreateCommandListV1(&context, &again);
    after = read_counts();
    CHECK(hr == S_OK && after.lists_recycled == before.lists_recycled + 1
            && after.lists_created == before.lists_created,
            "the next list reuses that memory through RecycleCreateCommandList");
    WineD3D11On12ExecuteCommandListV1(&out, &again);
    CHECK(read_counts().executed_draws == after.executed_draws + 2,
            "a list made from recycled memory runs what was recorded");

    /* A failure in RecycleCreateCommandList returns the block to the pool. */
    WineD3D11On12DestroyCommandListV1(&again);
    fail_next_list();
    init_list(&again);
    hr = WineD3D11On12CreateCommandListV1(&context, &again);
    CHECK(hr == E_OUTOFMEMORY && !again.hDrvCommandList && !again.runtimeState,
            "a failed recycled creation is reported and publishes nothing");
    before = read_counts();
    hr = WineD3D11On12CreateCommandListV1(&context, &again);
    after = read_counts();
    CHECK(hr == S_OK && after.lists_recycled == before.lists_recycled + 1,
            "the block a failed creation used is still reusable");
    WineD3D11On12DestroyCommandListV1(&again);

    /* ---- recording errors ---- */
    before = read_counts();
    record(&context, 1);
    {
        D3D10DDI_HDEVICE handle;

        handle.pDrvPrivate = context.hDrvContext;
        context.contextFuncs->pfnDraw(handle, MOCK_DEFERRED_FAIL, 0);
    }
    init_list(&again);
    hr = WineD3D11On12CreateCommandListV1(&context, &again);
    after = read_counts();
    CHECK(hr == E_INVALIDARG && !again.hDrvCommandList
            && after.abandoned == before.abandoned + 1,
            "an error reported while recording is returned instead of a list");
    hr = WineD3D11On12CreateCommandListV1(&context, &again);
    CHECK(hr == S_OK, "the context records cleanly after an abandoned list");
    before = read_counts();
    WineD3D11On12ExecuteCommandListV1(&out, &again);
    CHECK(read_counts().executed_draws == before.executed_draws,
            "nothing recorded before the error survives into the next list");
    WineD3D11On12DestroyCommandListV1(&again);

    /* ---- the pool is bounded ---- */
    for (i = 0; i < POOL_LIMIT + 2; ++i)
    {
        init_list(&lists[i]);
        WineD3D11On12CreateCommandListV1(&context, &lists[i]);
    }
    before = read_counts();
    for (i = 0; i < POOL_LIMIT + 2; ++i)
        WineD3D11On12DestroyCommandListV1(&lists[i]);
    after = read_counts();
    CHECK(after.lists_recycle_destroyed - before.lists_recycle_destroyed
            + after.lists_destroyed - before.lists_destroyed == POOL_LIMIT + 2
            && after.lists_destroyed - before.lists_destroyed >= 2,
            "past the pool limit, lists are destroyed outright");

    /* ---- another device's list ---- */
    mock_device_init(&other_device);
    other_device.support_device1 = 1;
    mock_queue_init(&other_queue, &other_device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    other_queues[0] = (IUnknown *)&other_queue.ID3D12CommandQueue_iface;
    memset(&other, 0, sizeof(other));
    other.size = sizeof(other);
    if (SUCCEEDED(WineD3D11On12OpenAdapterV1((IUnknown *)&other_device.ID3D12Device_iface,
            other_queues, 1, 0, &other)))
    {
        before = read_counts();
        CHECK(WineD3D11On12ExecuteCommandListV1(&other, &list) == E_INVALIDARG
                && read_counts().executes == before.executes,
                "a list is refused on a device that did not record it");
        WineD3D11On12CloseAdapterDeviceV1(&other);
    }
    else
        CHECK(0, "a second mock adapter opens");

    /* ---- context failure and destruction ---- */
    fail_next_context();
    init_context(&second);
    hr = WineD3D11On12CreateDeferredContextV1(&out, 0, &second);
    CHECK(hr == E_OUTOFMEMORY && !second.contextFuncs && !second.hDrvContext
            && !second.runtimeState, "a failed context creation publishes nothing");
    hr = WineD3D11On12CreateDeferredContextV1(&out, 0, &second);
    CHECK(hr == S_OK && second.hDrvContext != context.hDrvContext,
            "a second context is independent of the first");

    before = read_counts();
    CHECK(WineD3D11On12DestroyDeferredContextV1(&second) == S_OK
            && !second.contextFuncs && !second.hDrvContext,
            "destroying a context clears the handle");
    CHECK(WineD3D11On12DestroyDeferredContextV1(&second) == S_OK,
            "destroying a context twice is a no-op");
    after = read_counts();
    CHECK(after.contexts_destroyed == before.contexts_destroyed + 1,
            "a context reaches the driver's destroy once");
    init_list(&again);
    CHECK(WineD3D11On12CreateCommandListV1(&second, &again) == E_INVALIDARG,
            "a destroyed context cannot finish a list");

    /* A list outlives the context that recorded it. */
    CHECK(WineD3D11On12DestroyDeferredContextV1(&context) == S_OK, "the first context is destroyed");
    before = read_counts();
    CHECK(WineD3D11On12ExecuteCommandListV1(&out, &list) == S_OK
            && read_counts().executed_draws == before.executed_draws + 5,
            "a list still executes after its context is gone");

    /* ---- teardown ---- */
    init_context(&second);
    WineD3D11On12CreateDeferredContextV1(&out, 0, &second);
    before = read_counts();
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&out) == S_OK, "the device closes");
    after = read_counts();
    CHECK(after.contexts_destroyed == before.contexts_destroyed + 1
            && after.lists_destroyed == before.lists_destroyed + 1,
            "closing the device destroys the live context and list");
    CHECK(!list.hDrvCommandList && !list.runtimeState
            && !second.contextFuncs && !second.hDrvContext,
            "closing the device clears their handles");
    CHECK(WineD3D11On12DestroyCommandListV1(&list) == S_OK
            && WineD3D11On12DestroyDeferredContextV1(&second) == S_OK,
            "a late destroy after device teardown is a no-op");
    CHECK(!read_counts().bad, "the driver saw no malformed deferred-context call");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
