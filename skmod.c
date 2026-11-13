/* dinput8.dll proxy for SKEstivalVersus.exe (Steam, 64-bit): fixes applied in memory at every game
   start, each switched on/off in skmod.ini (next to this DLL). The exe on disk is never changed.
   Every exe edit first checks the original bytes; on a mismatch (other game version) that fix is
   skipped. Results of the last start: skmod.log.   Build: build.bat (MSVC x64). */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <dxgi.h>
#include <d3d11.h>
#include "version.h"

#define VA(x) (base + ((x) - 0x140000000ULL))
static BYTE *base;
static char dir[MAX_PATH], ini[MAX_PATH], logbuf[16384];
static CRITICAL_SECTION log_cs;
static volatile LONG tex_count, tex_mipped, tex_failed;   /* MipFix counters, written with the log */

static int opt(const char *key, int def) { return GetPrivateProfileIntA("SKMod", key, def, ini); }

static void say(const char *fmt, ...) {
    size_t n; va_list a;
    EnterCriticalSection(&log_cs);
    n = strlen(logbuf);
    va_start(a, fmt); vsnprintf(logbuf + n, sizeof logbuf - n, fmt, a); va_end(a);
    LeaveCriticalSection(&log_cs);
}

static void save_log(void) {
    char path[MAX_PATH]; FILE *f;
    sprintf_s(path, MAX_PATH, "%sskmod.log", dir);
    EnterCriticalSection(&log_cs);
    if (!fopen_s(&f, path, "w")) {
        fputs(logbuf, f);
        if (tex_count) fprintf(f, "Textures so far: %ld loaded, %ld got mipmaps (MipFix), %ld failed and kept as shipped\n", tex_count, tex_mipped, tex_failed);
        fclose(f);
    }
    LeaveCriticalSection(&log_cs);
}

static void put(void *p, const void *src, SIZE_T n) {
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, src, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

static int is(ULONGLONG va, const char *bytes, int n) { return !memcmp(VA(va), bytes, n); }

/* address a rip-relative operand points to: instruction at va, rel32 at va+off, instruction length len */
static BYTE *rip_target(ULONGLONG va, int off, int len) { return VA(va) + len + *(int *)(VA(va) + off); }

/* x64: a call rel32 from the exe can't reach this DLL, so it goes to a 'jmp [rip]' slot allocated
   within 2 GB of the exe */
static BYTE *slots; static int nslots;

static BYTE *near_jmp(void *fn) {
    BYTE *t;
    if (!slots) {
        ULONG_PTR a = ((ULONG_PTR)base & ~0xffffULL) - 0x10000;
        for (; !slots && a > (ULONG_PTR)base - 0x70000000; a -= 0x10000)
            slots = VirtualAlloc((void *)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!slots) return 0;
    }
    t = slots + 16 * nslots++;
    t[0] = 0xff; t[1] = 0x25; *(DWORD *)(t + 2) = 0; *(void **)(t + 6) = fn;
    return t;
}

static int call_at(ULONGLONG va, void *fn, int len) {    /* call fn + nop padding over len bytes */
    BYTE b[16], *p = VA(va), *t = near_jmp(fn);
    if (!t) return 0;
    b[0] = 0xe8; *(int *)(b + 1) = (int)(t - (p + 5));
    memset(b + 5, 0x90, len - 5);
    put(p, b, len);
    return 1;
}

/* ---- SkipIntro: boot scenes (global 'next scene'): CTaskFirstCheck (loads system data) -> 4 Marvelous
   movie -> 6 XSEED -> 5 Tamsoft -> 7 CRI -> 9 opening movie (only once the prologue was seen) ->
   10 prologue/title. Each logo task loads and frees its own data, so they can be left out. ---- */
void intro_stub(void);
ULONGLONG g_scene, g_flag;

static int fix_intro(int mode) {
    if (!is(0x1401abcca, "\xc7\x05", 2) || *(int *)VA(0x1401abcd0) != 4 ||
        !is(0x1401ab824, "\x0f\xb6\x05", 3) || !is(0x1401ab82b, "\xf6\xd8\x1b\xc9\x83\xc1\x0a\x89\x0d", 9) ||
        rip_target(0x1401abcca, 2, 10) != rip_target(0x1401ab832, 2, 6)) return 0;
    g_scene = (ULONGLONG)rip_target(0x1401abcca, 2, 10);
    g_flag = (ULONGLONG)rip_target(0x1401ab824, 3, 7);       /* 'prologue seen' (system data) */
    if (mode == 1) return call_at(0x1401abcca, intro_stub, 10);
    if (mode == 2) { int s = 10; put(VA(0x1401abcd0), &s, 4); }
    return 1;
}

/* ---- FrameRate: the in-game 30/60 setting picks from {15,20,30,60}; after Present a busy-wait limiter
   holds that rate. The game advances by the measured frame time in 1/60 s units (so 30 fps runs at
   the right speed), but clamped to >= 1, which would speed everything up above 60 fps. ---- */
static int fix_fps(int f) {
    static const int orig[4] = {15, 20, 30, 60};
    int t[2];
    if (memcmp(VA(0x140573af0), orig, sizeof orig) || !is(0x14026ccc2, "\x77\x04\x41\x0f\x28\xc9", 6) ||
        !is(0x14026ccb0, "\x41\x0f\x2f\xc9", 4)) return 0;
    if (f == 60) return 1;
    t[0] = f / 2; t[1] = f;
    put(VA(0x140573af8), t, sizeof t);
    put(VA(0x14026ccc2), "\xeb", 1);                       /* frame time below 1/60 s is kept */
    return 1;
}

/* ---- ShadowResolution: the shadows are Tamlib::CShadowVsmBlurCascade (variance shadow maps, blurred,
   cascaded), created once as 1536x1536 ('mov edx, 600h' before its init call); every target and
   viewport takes the size from there. A bigger map = smaller texels = less crawling/grain. ---- */
static int fix_shadow(int res) {
    if (!is(0x140045716, "\xba\x00\x06\x00\x00\x44\x8b\xc2", 8)) return 0;
    if (res != 1536) put(VA(0x140045717), &res, 4);
    return 1;
}

/* ---- MaxResolution: fullscreen always used the monitor size for the back buffer. Both places that
   set the fullscreen size (startup, applying the screen mode incl. alt-tab) use MaxResolution
   instead; the display mode is switched to it (IDXGISwapChain::ResizeTarget), where DSR/VSR takes
   over. Window and borderless are unchanged. ---- */
void res_start(void), res_apply(void);
ULONGLONG g_app;
int res_w, res_h;

static void cap_to_display(void) {
    DEVMODEA dm = {0}; DWORD i; int w = 0, h = 0;
    dm.dmSize = sizeof dm;
    for (i = 0; EnumDisplaySettingsA(0, i, &dm); i++)
        if ((int)(dm.dmPelsWidth * dm.dmPelsHeight) > w * h) { w = dm.dmPelsWidth; h = dm.dmPelsHeight; }
    if (!w) return;
    if (res_w > w || res_h > h) {
        say("MaxResolution: largest display mode is %dx%d (enable DSR/VSR in the driver for more)\n", w, h);
        res_w = w; res_h = h;
    }
}

static int fix_resolution(const char *s) {
    if (sscanf_s(s, "%dx%d", &res_w, &res_h) != 2 || res_w < 640 || res_h < 360 || res_w > 8192 || res_h > 8192) {
        res_w = res_h = 0; return -1;
    }
    if (!is(0x140271205, "\x89\x0d", 2) || !is(0x14027120b, "\x89\x05", 2) ||
        !is(0x1402712f7, "\x89\x0d", 2) || !is(0x1402712fd, "\x89\x15", 2) || !is(0x140271303, "\xc3", 1) ||
        rip_target(0x140271205, 2, 6) != rip_target(0x1402712f7, 2, 6) ||
        !is(0x140271e1a, "\x83\x7b\x50\x01", 4)) { res_w = res_h = 0; return 0; }
    cap_to_display();
    g_app = (ULONGLONG)rip_target(0x140271205, 2, 6) - 0x44;
    if (!call_at(0x140271205, res_start, 12) || !call_at(0x1402712f7, res_apply, 12)) { res_w = res_h = 0; return 0; }
    return 1;
}

static int fullscreen_mode(void) { return g_app && *(int *)(g_app + 0x50) == 1; }

static int fs_logged;
static void switch_mode(IDXGISwapChain *sc, UINT w, UINT h) {
    DXGI_MODE_DESC m = {0}; HRESULT hr;
    m.Width = w; m.Height = h;
    hr = IDXGISwapChain_ResizeTarget(sc, &m);
    if (fs_logged++ < 6) { say("MaxResolution: fullscreen %ux%u, display mode switch %s (%08lx)\n", w, h, SUCCEEDED(hr) ? "ok" : "FAILED", hr); save_log(); }
}

typedef HRESULT(STDMETHODCALLTYPE *SetFS)(IDXGISwapChain *, BOOL, IDXGIOutput *);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuf)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static SetFS setfs_orig;
static ResizeBuf rb_orig;

static HRESULT STDMETHODCALLTYPE setfs_hook(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *out) {
    HRESULT r = setfs_orig(sc, fs, out);
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(r) && fs && res_w && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d)))
        switch_mode(sc, d.BufferDesc.Width, d.BufferDesc.Height);
    return r;
}

static HRESULT STDMETHODCALLTYPE rb_hook(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    BOOL fs = FALSE; HRESULT r;
    if (res_w) fl |= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;   /* the game passed 0, dropping it */
    r = rb_orig(sc, n, w, h, f, fl);
    if (SUCCEEDED(r) && res_w && w && h && fullscreen_mode() &&
        SUCCEEDED(IDXGISwapChain_GetFullscreenState(sc, &fs, 0)) && fs) switch_mode(sc, w, h);
    return r;
}

/* ---- Anisotropy: every sampler goes through ID3D11Device::CreateSamplerState. The material samplers
   (0x140253a8d) are trilinear with the full mip range (MaxLOD = FLT_MAX), so unlike Valkyrie Drive
   no mip fix is needed; trilinear ones become anisotropic, point/comparison samplers are untouched. ---- */
typedef HRESULT(STDMETHODCALLTYPE *CreateSS)(ID3D11Device *, const D3D11_SAMPLER_DESC *, ID3D11SamplerState **);
static CreateSS ss_orig;
static int aniso_level;
static volatile LONG ss_count, ss_aniso;

static HRESULT STDMETHODCALLTYPE ss_hook(ID3D11Device *dev, const D3D11_SAMPLER_DESC *in, ID3D11SamplerState **out) {
    D3D11_SAMPLER_DESC d;
    InterlockedIncrement(&ss_count);
    if (!in || in->Filter != D3D11_FILTER_MIN_MAG_MIP_LINEAR) return ss_orig(dev, in, out);
    d = *in;
    d.Filter = D3D11_FILTER_ANISOTROPIC; d.MaxAnisotropy = aniso_level;
    InterlockedIncrement(&ss_aniso);
    return ss_orig(dev, &d, out);
}

/* ---- textures: the game loads them with D3DX11CreateTextureFromMemory (no load info) ---- */
typedef struct {                       /* D3DX11_IMAGE_INFO (d3dx11tex.h, not in the Windows SDK) */
    UINT Width, Height, Depth, ArraySize, MipLevels, MiscFlags; DXGI_FORMAT Format;
    D3D11_RESOURCE_DIMENSION ResourceDimension; int ImageFileFormat;
} ImgInfo;
typedef struct {                       /* D3DX11_IMAGE_LOAD_INFO */
    UINT Width, Height, Depth, FirstMipLevel, MipLevels; D3D11_USAGE Usage;
    UINT BindFlags, CpuAccessFlags, MiscFlags; DXGI_FORMAT Format; UINT Filter, MipFilter; ImgInfo *pSrcInfo;
} LoadInfo;
#define DX_DEFAULT ((UINT)-1)
#define DX_FROM_FILE ((UINT)-3)
typedef HRESULT(WINAPI *InfoFn)(const void *, SIZE_T, void *, ImgInfo *, HRESULT *);
typedef HRESULT(WINAPI *TexFn)(ID3D11Device *, const void *, SIZE_T, LoadInfo *, void *, ID3D11Resource **, HRESULT *);
static InfoFn info_fn;
static TexFn tex_orig;

/* MipFix: most stage/object textures ship as single-level DDS (e.g. 3649 of the 4.6k DXT textures in
   Model/Field) or with a short chain (e.g. 4 levels on 1024px), so they shimmer at a distance whatever
   the sampler allows. Those get a full mip chain built by D3DX at load (same format; box filter). Cube maps and tiny textures are left as they are. */
static UINT full_mips(UINT w, UINT h) { UINT n = 1, m = w > h ? w : h; while (m > 1) { m >>= 1; n++; } return n; }

static HRESULT WINAPI tex_hook(ID3D11Device *dev, const void *src, SIZE_T n, LoadInfo *li, void *pump, ID3D11Resource **tex, HRESULT *hr2) {
    ImgInfo ii; HRESULT r;
    InterlockedIncrement(&tex_count);
    if (!li && SUCCEEDED(info_fn(src, n, 0, &ii, 0)) && ii.MipLevels < full_mips(ii.Width, ii.Height) - 2 && ii.ArraySize == 1 &&
        ii.ResourceDimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D && !(ii.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) &&
        (ii.Width >= 16 || ii.Height >= 16)) {
        LoadInfo l = {DX_FROM_FILE, DX_FROM_FILE, DX_FROM_FILE, 0, 0, (D3D11_USAGE)DX_DEFAULT, DX_DEFAULT, DX_DEFAULT,
                      DX_DEFAULT, (DXGI_FORMAT)DX_FROM_FILE, DX_DEFAULT, DX_DEFAULT, 0};
        if (SUCCEEDED(r = tex_orig(dev, src, n, &l, pump, tex, hr2))) InterlockedIncrement(&tex_mipped);
        else { InterlockedIncrement(&tex_failed); r = tex_orig(dev, src, n, li, pump, tex, hr2); }   /* as shipped */
    } else r = tex_orig(dev, src, n, li, pump, tex, hr2);
    if (tex_count % 200 == 0) save_log();
    return r;
}

/* ---- the device and swap chain come from the exe's D3D11CreateDeviceAndSwapChain import ---- */
typedef HRESULT(WINAPI *CreateDevSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                     const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                     ID3D11DeviceContext **);
static CreateDevSC dev_orig;

/* ---- AOBlur: the SSAO pass ('SSAO', HBAO with a per-pixel random rotation) goes straight into the
   composite ('AOCP': screen * ao^2) without any blur, so its noise shows as grain in creases and
   contact areas. The composite is replaced by one that first blurs the AO 5x5, weighted by the depth
   the HBAO pass stores next to it (.y), so the blur doesn't cross object edges. ---- */
static const char aocp_hlsl[] =
    "Texture2D screenTex : register(t0); Texture2D ssaoTex : register(t1); SamplerState samp0 : register(s0);\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float w, h; ssaoTex.GetDimensions(w, h); float2 px = float2(1.0 / w, 1.0 / h);\n"
    "  float2 c = ssaoTex.SampleLevel(samp0, uv, 0).xy; float sum = 0, wsum = 0;\n"
    "  [unroll] for (int y = -2; y <= 2; y++) [unroll] for (int x = -2; x <= 2; x++) {\n"
    "    float2 s = ssaoTex.SampleLevel(samp0, uv + float2(x, y) * px, 0).xy;\n"
    "    float wt = saturate(1.0 - abs(s.y - c.y) / (0.03 * abs(c.y) + 1e-4));\n"
    "    sum += s.x * wt; wsum += wt; }\n"
    "  float ao = sum / max(wsum, 1e-4);\n"
    "  return float4(screenTex.Sample(samp0, uv).rgb * ao * ao, 1.0); }\n";
typedef HRESULT(WINAPI *CompileFn)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                   UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT(STDMETHODCALLTYPE *CreatePS)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static CreatePS ps_orig;
static int aoblur_on;

static HRESULT STDMETHODCALLTYPE ps_hook(ID3D11Device *dev, const void *code, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11PixelShader **ps) {
    const BYTE *aocp = VA(0x1404d28e0);
    if (code && len == *(DWORD *)(aocp + 24) && !memcmp(code, aocp, len)) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : 0;
        ID3DBlob *b = 0, *err = 0; HRESULT r = E_FAIL;
        if (compile && SUCCEEDED(compile(aocp_hlsl, sizeof aocp_hlsl - 1, "aoblur", 0, 0, "main", "ps_4_0", 0, 0, &b, &err)))
            r = ps_orig(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), cl, ps);
        say("AOBlur: SSAO composite shader %s\n", SUCCEEDED(r) ? "replaced (AO blurred)" : "NOT replaced (compile failed), original kept");
        save_log();
        if (b) ID3D10Blob_Release(b);
        if (err) ID3D10Blob_Release(err);
        if (SUCCEEDED(r)) return r;
    }
    return ps_orig(dev, code, len, cl, ps);
}

static void hook_slot(void **vt, int i, void *hook, void **orig) {
    if (vt[i] != hook) { *orig = vt[i]; put(&vt[i], &hook, sizeof hook); }
}

static HRESULT WINAPI dev_hook(IDXGIAdapter *ad, D3D_DRIVER_TYPE type, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL *lv,
                               UINT n, UINT sdk, const DXGI_SWAP_CHAIN_DESC *scd, IDXGISwapChain **sc, ID3D11Device **dev,
                               D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    HRESULT r = dev_orig(ad, type, sw, fl, lv, n, sdk, scd, sc, dev, got, ctx);
    if (SUCCEEDED(r) && dev && *dev) {
        if (aniso_level)
            hook_slot(*(void ***)*dev, 23, (void *)ss_hook, (void **)&ss_orig);      /* CreateSamplerState */
        if (aoblur_on)
            hook_slot(*(void ***)*dev, 15, (void *)ps_hook, (void **)&ps_orig);      /* CreatePixelShader */
        if (res_w && sc && *sc) {
            hook_slot(*(void ***)*sc, 10, (void *)setfs_hook, (void **)&setfs_orig);  /* SetFullscreenState */
            hook_slot(*(void ***)*sc, 13, (void *)rb_hook, (void **)&rb_orig);        /* ResizeBuffers */
        }
        say("D3D11 device created: %ux%u, %s\n", scd ? scd->BufferDesc.Width : 0, scd ? scd->BufferDesc.Height : 0,
            fullscreen_mode() ? "fullscreen" : "window/borderless");
        save_log();
    }
    return r;
}

static void **iat_slot(const char *dll, const char *func) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress);
    for (; dd->VirtualAddress && d->Name; d++) {
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(base + d->OriginalFirstThunk);
        DWORD i;
        if (_stricmp((char *)base + d->Name, dll) || !d->OriginalFirstThunk) continue;
        for (i = 0; names[i].u1.AddressOfData; i++)
            if (!(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) &&
                !strcmp(((IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData))->Name, func))
                return (void **)(base + d->FirstThunk) + i;
    }
    return 0;
}

/* ---- startup ---- */
static void report(const char *key, int val, int ok) {
    say("%s=%d: %s\n", key, val, !ok ? "SKIPPED - unexpected exe bytes (other game version?)" : val ? "on" : "off");
}

static void install(HMODULE self) {
    IMAGE_NT_HEADERS *nt; char *p, s[32]; int v; void **slot;
    InitializeCriticalSection(&log_cs);
    base = (BYTE *)GetModuleHandleA(0);
    GetModuleFileNameA(self, dir, MAX_PATH);
    p = strrchr(dir, '\\'); p[1] = 0;
    sprintf_s(ini, MAX_PATH, "%sskmod.ini", dir);
    say(MOD_NAME " " MOD_VER_STR " (dinput8.dll), options from %s\n", ini);
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    say("Game exe time stamp %08lx%s\n", nt->FileHeader.TimeDateStamp,
        nt->FileHeader.TimeDateStamp == 0x58f075f7 ? " (Steam build the mod was made for)" : " (UNKNOWN build: byte checks decide)");

    v = opt("Anisotropy", 16);
    if (v >= 0 && v <= 16) { aniso_level = v; say("Anisotropy=%d: %s\n", v, v ? "on" : "off"); }
    else say("Anisotropy=%d: SKIPPED - must be 0..16\n", v);
    v = opt("SkipIntro", 2);
    if (v >= 0 && v <= 2) report("SkipIntro", v, fix_intro(v)); else say("SkipIntro=%d: SKIPPED - must be 0..2\n", v);
    v = opt("AOBlur", 1);
    aoblur_on = v && !memcmp(VA(0x1404d28e0), "DXBC", 4) && *(DWORD *)VA(0x1404d28f8) < 4096;
    say("AOBlur=%d: %s\n", v, !v ? "off" : aoblur_on ? "on" : "SKIPPED - unexpected exe bytes (other game version?)");
    v = opt("ShadowResolution", 1536);
    if (v >= 512 && v <= 8192) say("ShadowResolution=%d: %s\n", v, fix_shadow(v) ? "on" : "SKIPPED - unexpected exe bytes (other game version?)");
    else say("ShadowResolution=%d: SKIPPED - must be 512..8192\n", v);
    v = opt("FrameRate", 60);
    if (v >= 60 && v <= 360) say("FrameRate=%d: %s\n", v, fix_fps(v) ? "on" : "SKIPPED - unexpected exe bytes (other game version?)");
    else say("FrameRate=%d: SKIPPED - must be 60..360\n", v);
    GetPrivateProfileStringA("SKMod", "MaxResolution", "0", s, sizeof s, ini);
    if (atoi(s) == 0) say("MaxResolution=0: off\n");
    else if ((v = fix_resolution(s)) < 0) say("MaxResolution=%s: SKIPPED - must be WIDTHxHEIGHT (e.g. 3840x2160) or 0\n", s);
    else if (!v) say("MaxResolution=%s: SKIPPED - unexpected exe bytes (other game version?)\n", s);
    else say("MaxResolution=%dx%d: on (Fullscreen screen mode)\n", res_w, res_h);

    v = opt("MipFix", 1);
    if (v && (slot = iat_slot("d3dx11_43.dll", "D3DX11GetImageInfoFromMemory"))) info_fn = (InfoFn)*slot;
    if (info_fn && (slot = iat_slot("d3dx11_43.dll", "D3DX11CreateTextureFromMemory"))) {
        void *h = (void *)tex_hook; tex_orig = (TexFn)*slot; put(slot, &h, sizeof h);
    }
    say("MipFix=%d: %s\n", v, !v ? "off" : tex_orig ? "on" : "SKIPPED - D3DX11 texture import not found");
    if ((slot = iat_slot("d3d11.dll", "D3D11CreateDeviceAndSwapChain"))) {
        void *h = (void *)dev_hook; dev_orig = (CreateDevSC)*slot; put(slot, &h, sizeof h);
    } else say("D3D11 import not found: Anisotropy and the MaxResolution display switch inactive\n");
    save_log();
}

typedef HRESULT(WINAPI *DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u) {
    static DI8Create real;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH); strcat_s(path, MAX_PATH, "\\dinput8.dll");
        real = (DI8Create)GetProcAddress(LoadLibraryA(path), "DirectInput8Create");
        if (!real) return E_FAIL;
    }
    return real(h, v, r, o, u);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID res) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(self); install(self); }
    if (reason == DLL_PROCESS_DETACH) {
        if (ss_orig) say("Samplers: %ld created, %ld made anisotropic\n", ss_count, ss_aniso);
        save_log();
    }
    return TRUE;
}
