/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Self-test for the clean-room DDI layout harness.
 *
 * No DDI group is declared yet.  This test guards the harness those groups
 * will depend on: it proves the WINE_DDI_ASSERT_* macros compile in both C and
 * C++, and that their compile-time answers agree with the layout an actual
 * object has at run time.  A harness that silently evaluated to true would
 * make every future DDI offset assertion worthless.
 *
 * Build (both languages must succeed):
 *   x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
 *       -Igptk-d3d11/ddi -o d3d11ddilayout.exe tests/d3d11ddilayout.c
 *   x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror \
 *       -Igptk-d3d11/ddi -x c++ -o d3d11ddilayoutxx.exe tests/d3d11ddilayout.c
 */
#include <stdio.h>
#include <string.h>

#include "wine_d3d11ddi.h"

/* A reference structure whose layout follows from the Win64 C ABI alone. */
struct layout_probe
{
    UINT32 first;
    UINT32 second;
    UINT64 third;
    void *fourth;
    UINT32 fifth;
};

WINE_DDI_ASSERT_STANDARD_LAYOUT(struct layout_probe);
WINE_DDI_ASSERT_FIELD(struct layout_probe, first, 0);
WINE_DDI_ASSERT_FIELD(struct layout_probe, second, 4);
WINE_DDI_ASSERT_FIELD(struct layout_probe, third, 8);
WINE_DDI_ASSERT_FIELD_SIZE(struct layout_probe, third, 8);
WINE_DDI_ASSERT_ALIGN(struct layout_probe, 8);
WINE_DDI_STATIC_ASSERT(sizeof(void *) != 8
        || offsetof(struct layout_probe, fourth) == 16,
        "layout_probe.fourth has an unexpected offset");
WINE_DDI_STATIC_ASSERT(sizeof(void *) != 8
        || offsetof(struct layout_probe, fifth) == 24,
        "layout_probe.fifth has an unexpected offset");
WINE_DDI_STATIC_ASSERT(sizeof(void *) != 8
        || sizeof(struct layout_probe) == 32,
        "layout_probe has an unexpected size");

static int failures;

static void check_offset(const char *field, unsigned long asserted,
        unsigned long actual)
{
    if (asserted == actual)
    {
        printf("[ ok ] layout_probe.%s at offset %lu\n", field, actual);
        return;
    }
    printf("[fail] layout_probe.%s: harness says %lu, the object says %lu\n",
            field, asserted, actual);
    ++failures;
}

int main(void)
{
    struct layout_probe probe;
    const char *base = (const char *)&probe;

    memset(&probe, 0, sizeof(probe));

    check_offset("first", (unsigned long)offsetof(struct layout_probe, first),
            (unsigned long)((const char *)&probe.first - base));
    check_offset("second", (unsigned long)offsetof(struct layout_probe, second),
            (unsigned long)((const char *)&probe.second - base));
    check_offset("third", (unsigned long)offsetof(struct layout_probe, third),
            (unsigned long)((const char *)&probe.third - base));
    check_offset("fourth", (unsigned long)offsetof(struct layout_probe, fourth),
            (unsigned long)((const char *)&probe.fourth - base));
    check_offset("fifth", (unsigned long)offsetof(struct layout_probe, fifth),
            (unsigned long)((const char *)&probe.fifth - base));

    if (failures)
    {
        printf("RESULT: %d DDI layout harness failure(s)\n", failures);
        return 1;
    }
    printf("RESULT: DDI layout harness agrees with the compiled ABI\n");
    return 0;
}
