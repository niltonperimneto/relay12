/* Fuzzing Harness for D3D11 UM DDI and Shim APIs */
/* Designed for libFuzzer/AFL++ integration */

#include <stddef.h>
#include <stdint.h>
#include "../relay12-d3d11/d3d11on12core.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* We need at least enough data to construct mock parameters */
    if (size < 64)
        return 0;

    /* Extract fuzz data to simulate arbitrary user application inputs */
    UINT flags = *(UINT*)(data);
    UINT numQueues = *(UINT*)(data + 4);
    UINT nodeMask = *(UINT*)(data + 8);
    UINT featureLevelsCount = *(UINT*)(data + 12);

    /* Clamp counts to prevent OOM in the fuzzer itself,
       we are testing the driver logic, not system memory exhaustion */
    numQueues %= 16;
    featureLevelsCount %= 16;

    /* Construct garbage COM pointers using the fuzzer feed */
    IUnknown* mockDevice = (IUnknown*)(data + 16);

    IUnknown** mockQueues = NULL;
    if (numQueues > 0 && numQueues <= (size - 64) / sizeof(void*)) {
        mockQueues = (IUnknown**)(data + 64);
    }

    D3D_FEATURE_LEVEL* mockFeatureLevels = NULL;
    size_t queueBytes = numQueues * sizeof(void*);
    size_t featureOffset = 64 + queueBytes;
    if (featureLevelsCount > 0 && featureOffset <= size &&
        featureLevelsCount <= (size - featureOffset) / sizeof(D3D_FEATURE_LEVEL)) {
        mockFeatureLevels = (D3D_FEATURE_LEVEL*)(data + featureOffset);
    }

    void* outDevice = NULL;
    void* outContext = NULL;
    D3D_FEATURE_LEVEL outFeatureLevel;

    /* Execute the D3D11On12 boundary with fuzzed input to test for
       null-pointer dereferences, integer overflows, or unhandled exceptions */
    HRESULT hr = WineD3D11On12CreateDeviceV1(
        mockDevice,
        flags,
        mockFeatureLevels,
        featureLevelsCount,
        (IUnknown *const *)mockQueues,
        numQueues,
        nodeMask,
        &outDevice,
        &outContext,
        &outFeatureLevel
    );

    (void)hr;

    return 0; /* Tell libFuzzer we survived a cycle without crashing */
}
