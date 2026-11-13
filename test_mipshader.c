/* Check for mipshader.h: patches every pixel shader embedded in the game exe and creates the patched
   ones on a WARP D3D11 device (which validates bytecode and checksum).
   Build/run: test.bat (exe path as argument, default ..\SKEstivalVersus.exe). */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <d3d11.h>
#include "mipshader.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "..\\SKEstivalVersus.exe";
    FILE *f; BYTE *buf; long n, i; int seen = 0, patched = 0, failed = 0, reads = 0;
    ID3D11Device *dev; ID3D11PixelShader *ps;
    if (fopen_s(&f, path, "rb")) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc(n); fread(buf, 1, n, f); fclose(f);
    if (FAILED(D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0, 0, 0, 0, D3D11_SDK_VERSION, &dev, 0, 0))) { printf("no WARP device\n"); return 1; }
    for (i = 0; i + 32 < n; i++) {
        BYTE *p; int k;
        if (memcmp(buf + i, "DXBC", 4) || *(DWORD *)(buf + i + 24) > (DWORD)(n - i)) continue;
        seen++;
        if (!(p = mip_shader_patch(buf + i, n - i, &k))) continue;
        if (SUCCEEDED(ID3D11Device_CreatePixelShader(dev, p, *(DWORD *)(p + 24), 0, &ps))) { patched++; reads += k; ID3D11PixelShader_Release(ps); }
        else { failed++; printf("FAILED: shader at file offset %lx\n", i); }
        free(p);
    }
    printf("%d shaders, %d patched (%d texture reads) and accepted by D3D11, %d rejected\n", seen, patched, reads, failed);
    return failed || !patched;
}
