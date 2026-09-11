// SPDX-License-Identifier: GPL-3.0-only
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "d3d11on12.dll";
    HMODULE module = LoadLibraryA(path);
    if (!module)
    {
        fprintf(stderr, "LoadLibraryA failed: %lu\n", GetLastError());
        return 1;
    }
    if (!GetProcAddress(module, "OpenAdapter_D3D11On12"))
    {
        fprintf(stderr, "OpenAdapter_D3D11On12 is missing\n");
        FreeLibrary(module);
        return 2;
    }
    FreeLibrary(module);
    return 0;
}
