/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Host dispatch overhead against the mock driver.
 *
 * The mock's draw and flush are an interlocked increment each, so what this
 * times is everything between a caller and the driver's function table: the
 * core's entry-point validation and frame lock, and -- when RELAY12_TELEMETRY
 * is set -- the timing proxies wrapped around the draw and flush slots.  CI
 * runs it with telemetry off and on and publishes both, which is what shows
 * the proxies cost nothing when they are not installed and little when they
 * are.
 *
 * The figures are informational.  A shared CI runner under Wine is far too
 * noisy for a threshold, so the only failures here are correctness ones: every
 * call must reach the driver exactly once whether or not it went through a
 * proxy, and a flush must still report the driver's submitted result.
 *
 * Output is one "overhead: <metric>=<value>" line per figure so the workflow
 * can lift them into the step summary without parsing prose.
 */

#include <stdio.h>
#include <string.h>

#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"
#include "d3d11on12mocks.h"

#define DRAW_ITERATIONS 200000
#define FLUSH_ITERATIONS 50000

static int failures;

static void check(int condition, const char *what)
{
    if (condition)
    {
        printf("[ ok ] %s\n", what);
    }
    else
    {
        printf("[fail] %s\n", what);
        ++failures;
    }
}

static LONGLONG now(void)
{
    LARGE_INTEGER counter;

    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

static void report(const char *metric, LONGLONG ticks, LONGLONG frequency,
        LONG iterations)
{
    printf("overhead: %s=%.1f\n", metric,
            (double)ticks * 1e9 / (double)frequency / (double)iterations);
}

int main(void)
{
    typedef LONG (WINAPI *get_count_fn)(void);
    typedef void (WINAPI *get_extended_draw_counts_fn)(LONG *, LONG *, LONG *);
    WineD3D11On12AdapterDevice out;
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queue_objects[1];
    D3D10DDI_HDEVICE handle;
    LARGE_INTEGER frequency;
    HMODULE mock_driver;
    get_count_fn get_draw_count, get_flush_count;
    get_extended_draw_counts_fn get_extended_draw_counts;
    LONG indexed, instanced, indexed_instanced;
    LONG draws_before, flushes_before;
    LONGLONG start;
    BOOL submitted, all_submitted;
    wchar_t telemetry[2] = {0};
    HRESULT hr;
    LONG i;

    QueryPerformanceFrequency(&frequency);
    printf("overhead: telemetry=%s\n",
            GetEnvironmentVariableW(L"RELAY12_TELEMETRY", telemetry, 2) == 1
            && telemetry[0] == L'1' ? "on" : "off");

    mock_driver = LoadLibraryW(L"d3d11on12.dll");
    get_draw_count = mock_driver ? (get_count_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetDrawCount") : NULL;
    get_flush_count = mock_driver ? (get_count_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetFlushCount") : NULL;
    get_extended_draw_counts = mock_driver
            ? (get_extended_draw_counts_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverGetExtendedDrawCounts") : NULL;
    check(get_draw_count && get_flush_count && get_extended_draw_counts,
          "the mock driver's counters are exported");
    if (!get_draw_count || !get_flush_count || !get_extended_draw_counts)
        return 1;

    mock_device_init(&device);
    device.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queue_objects[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    memset(&out, 0, sizeof(out));
    out.size = sizeof(out);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 1, 0, &out);
    check(hr == S_OK && out.deviceFuncs, "the mock adapter opens");
    if (FAILED(hr) || !out.deviceFuncs)
        return 1;

    /* The table a frontend calls straight through: with telemetry on, this
     * is the proxy plus the driver and nothing else. */
    handle.pDrvPrivate = out.hDrvDevice;
    draws_before = get_draw_count();
    start = now();
    for (i = 0; i < DRAW_ITERATIONS; ++i)
        out.deviceFuncs->pfnDraw(handle, 3, 0);
    report("table_draw_ns", now() - start, frequency.QuadPart,
            DRAW_ITERATIONS);
    check(get_draw_count() - draws_before == DRAW_ITERATIONS,
          "every table draw reaches the driver exactly once");

    /* The core's own entry point: argument checks and the frame lock on top
     * of the same table call. */
    draws_before = get_draw_count();
    start = now();
    for (i = 0; i < DRAW_ITERATIONS; ++i)
        WineD3D11On12DrawAdapterDeviceV1(&out, 3, 0);
    report("entry_draw_ns", now() - start, frequency.QuadPart,
            DRAW_ITERATIONS);
    check(get_draw_count() - draws_before == DRAW_ITERATIONS,
          "every entry-point draw reaches the driver exactly once");

    start = now();
    for (i = 0; i < DRAW_ITERATIONS; ++i)
        WineD3D11On12DispatchDrawV1(&out, WINE_D3D11ON12_DRAW_INSTANCED,
                3, 1, 0, 0, 0);
    report("entry_draw_instanced_ns", now() - start, frequency.QuadPart,
            DRAW_ITERATIONS);
    get_extended_draw_counts(&indexed, &instanced, &indexed_instanced);
    check(instanced == DRAW_ITERATIONS && indexed == 0
            && indexed_instanced == 0,
          "every instanced draw reaches its own slot exactly once");

    flushes_before = get_flush_count();
    all_submitted = TRUE;
    start = now();
    for (i = 0; i < FLUSH_ITERATIONS; ++i)
    {
        submitted = FALSE;
        WineD3D11On12FlushAdapterDeviceV1(&out, 0, 0, &submitted);
        all_submitted &= submitted;
    }
    report("entry_flush_ns", now() - start, frequency.QuadPart,
            FLUSH_ITERATIONS);
    check(get_flush_count() - flushes_before == FLUSH_ITERATIONS,
          "every flush reaches the driver exactly once");
    check(all_submitted, "every flush reports the driver's submitted result");

    /* Closing the device is what emits the telemetry report, so the
     * workflow's check of that report depends on this call. */
    hr = WineD3D11On12CloseAdapterDeviceV1(&out);
    check(hr == S_OK, "the mock adapter closes");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
