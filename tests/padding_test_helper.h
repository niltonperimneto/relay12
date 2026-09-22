#ifndef WINE_D3D11_PADDING_TEST_H
#define WINE_D3D11_PADDING_TEST_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Phase 3: Dirty Memory Injection for Padding Traps */

/* Allocates memory filled with 0xCC to detect implicit padding */
static inline void *malloc_dirty(size_t size)
{
    void *ptr = malloc(size);
    if (ptr)
        memset(ptr, 0xCC, size);
    return ptr;
}

/* Scans memory for 0xCC bytes to check if implicit padding leaked.
 *
 * Returns the count and leaves the verdict to the caller, because both
 * answers are wanted somewhere: a zeroed DDI argument struct must report no
 * leaks, and the reference probe is deliberately left un-zeroed so that
 * finding its tail padding proves the trap still works.
 *
 * The per-byte lines are therefore reported as [info], not [fail].  Printing
 * them as failures made a passing trap look like four broken checks in the CI
 * log and cost a diagnosis; only the caller knows which way round it is. */
static inline int check_uninitialized_padding(const char *name, const void *ptr, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)ptr;
    size_t i;
    int leaks = 0;

    for (i = 0; i < size; ++i)
    {
        if (bytes[i] == 0xCC)
        {
            printf("[info] uninitialized padding byte in %s at offset %lu\n", name, (unsigned long)i);
            leaks++;
        }
    }

    if (leaks == 0)
    {
        printf("[ ok ] %s has no uninitialized padding bytes\n", name);
    }
    return leaks;
}

#endif
