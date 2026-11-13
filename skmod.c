/* dinput8.dll proxy for SKEstivalVersus.exe (Steam, 64-bit): fixes applied in memory at every game
   start, each switched on/off in skmod.ini (next to this DLL). The exe on disk is never changed.
   Every exe edit first checks the original bytes; on a mismatch (other game version) that fix is
   skipped. Results of the last start: skmod.log.   Build: build.bat (MSVC x64). */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>
#include <dxgi.h>
#include <d3d11.h>
#include "version.h"
#include "mipshader.h"
#include "evsm.h"

#define VA(x) (base + ((x) - 0x140000000ULL))
static BYTE *base;
static char dir[MAX_PATH], ini[MAX_PATH], logbuf[16384];
static CRITICAL_SECTION log_cs;
static volatile LONG tex_count, tex_mipped, tex_failed, ps_mip, ps_mip_failed;   /* MipFix counters, written with the log */

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
        if (ps_mip || ps_mip_failed) fprintf(f, "Material shaders so far: %ld patched (MipFix/ShadowNoiseFix), %ld refused and kept original\n", ps_mip, ps_mip_failed);
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
   the right speed), but clamped to >= 1, which would speed everything up above 60 fps.
   Model instances advance their object and material animations by a fixed default speed of 1.0 per
   frame unless a caller sets it from the frame time (measured with TraceAnim/TraceSea: the menu sea
   scrolled 0.0465/s at 120 fps vs 0.0235/s at 60). obj_adv/mat_adv scale a default speed by the
   frame time (stubs.asm).
   Characters in event scenes (entries, transformations, defeat moves, their Library replays) are
   updated with a literal step of 1.0 per frame (movss xmm1, [1.0] before call [rax+40h], found with
   TraceMotion); those two loads now read the game's frame-time step (1405c63ac, which also follows
   slow motion) like normal gameplay. ---- */
void obj_adv(void), mat_adv(void);
ULONGLONG g_frametime;

static int fix_fps(int f) {
    static const int orig[4] = {15, 20, 30, 60};
    static const ULONGLONG ev_step[2] = {0x1400e66ce, 0x1400e6b49};
    int t[2], i;
    if (memcmp(VA(0x140573af0), orig, sizeof orig) || !is(0x14026ccc2, "\x77\x04\x41\x0f\x28\xc9", 6) ||
        !is(0x14026ccb0, "\x41\x0f\x2f\xc9", 4) || !is(0x14026ccd2, "\xf3\x0f\x11\x0d", 4) ||
        !is(0x14004120d, "\xf3\x0f\x10\x4b\x64\xf3\x0f\x58\x4b\x60", 10) ||
        !is(0x140041240, "\xf3\x0f\x10\x8b\x84\x00\x00\x00\x48\x8b\x43\x78", 12)) return 0;
    for (i = 0; i < 2; i++)                                 /* movss xmm1, [rip -> 1.0] */
        if (!is(ev_step[i], "\xf3\x0f\x10\x0d", 4) || rip_target(ev_step[i], 4, 8) != VA(0x14057643c) ||
            *(float *)VA(0x14057643c) != 1.0f) return 0;
    if (f == 60) return 1;
    t[0] = f / 2; t[1] = f;
    put(VA(0x140573af8), t, sizeof t);
    put(VA(0x14026ccc2), "\xeb", 1);                       /* frame time below 1/60 s is kept */
    g_frametime = (ULONGLONG)rip_target(0x14026ccd2, 4, 8);   /* where the limiter stores it */
    for (i = 0; i < 2; i++) {                               /* event characters: game step instead of 1.0 */
        int d = (int)(VA(0x1405c63ac) - (VA(ev_step[i]) + 8));
        put(VA(ev_step[i] + 4), &d, 4);
    }
    return call_at(0x14004120d, obj_adv, 10) && call_at(0x140041240, mat_adv, 8);
}

/* ---- ShadowResolution: the shadows are Tamlib::CShadowVsmBlurCascade (variance shadow maps, blurred,
   cascaded), created once as 1536x1536 ('mov edx, 600h' before its init call); every target and
   viewport takes the size from there. A bigger map = smaller texels = less crawling/grain. ---- */
static int fix_shadow(int res) {
    if (!is(0x140045716, "\xba\x00\x06\x00\x00\x44\x8b\xc2", 8)) return 0;
    if (res != 1536) put(VA(0x140045717), &res, 4);
    return 1;
}

/* ---- NearClip: the cameras use near 0.1 / far 10000 with a 24-bit depth buffer. The menu and many
   in-game views are telephoto shots from ~35 m, where depth can only tell surfaces apart ~0.7 mm or
   more apart: swimsuits and clothes sitting on the body z-fight with it (flickering skin-coloured
   blotches; seen with a RenderDoc pixel history). Depth precision grows with the near distance; a
   fixed higher near cut close-ups, so it follows the zoom instead: the getters GetNear (140243150)
   and GetNearFar (140243170), which every consumer uses, return near * clamp(40deg / fov, 1, NearClip).
   Normal shots keep 0.1; the 4.6deg menu telephoto gets ~0.87. Depth of field's own fixed near
   (global 140e51270, tuned for 0.1) gets the same factor. Shadow cameras are excluded (stubs.asm). ---- */
void near_get(void), near_getpair(void);
float fov_ref = 0.6981317f, near_max, one_f = 1.0f;
ULONGLONG shadow_vt, g_dofnear, shadow_ret[4];   /* return addresses of the shadow code's GetNear calls */

static int jmp_at(ULONGLONG va, void *fn) {
    BYTE b[5], *p = VA(va), *t = near_jmp(fn);
    if (!t) return 0;
    b[0] = 0xe9; *(int *)(b + 1) = (int)(t - (p + 5));
    put(p, b, 5);
    return 1;
}

static int fix_near(float maxscale) {
    if (!is(0x140243150, "\xf3\x0f\x10\x81\x60\x02\x00\x00\xc3", 9) ||
        !is(0x140243170, "\x8b\x81\x60\x02\x00\x00\x89\x02\x8b\x81\x64\x02\x00\x00\x41\x89\x00\xc3", 18) ||
        !is(0x14026fdf4, "\xf3\x0f\x10\x1d", 4) ||
        *(ULONGLONG *)VA(0x1404ec718 + 0xa8) != (ULONGLONG)VA(0x140243150)) return 0;   /* CShadowCamera vtable */
    /* the shadow cascades are fitted to the view camera's near (CCascadeVariantShadowRender and the
       cascade setup): they keep the original one, or close shadows would drop out */
    static const ULONGLONG calls[4] = {0x14026003d, 0x140260192, 0x140046065, 0x140046b5f};
    int i;
    for (i = 0; i < 4; i++) {                                     /* call qword ptr [rax + 0a8h] */
        if (!is(calls[i], "\xff\x90\xa8\x00\x00\x00", 6)) return 0;
        shadow_ret[i] = (ULONGLONG)VA(calls[i] + 6);
    }
    near_max = maxscale; shadow_vt = (ULONGLONG)VA(0x1404ec718);
    g_dofnear = (ULONGLONG)rip_target(0x14026fdf4, 4, 8);
    return jmp_at(0x140243150, near_get) && jmp_at(0x140243170, near_getpair);
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
static float evsm_k;

static HRESULT STDMETHODCALLTYPE ps_hook(ID3D11Device *dev, const void *code, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11PixelShader **ps) {
    const BYTE *aocp = VA(0x1404d28e0);
    BYTE *p = 0, *e = 0; int k = 0; DWORD el; HRESULT r;
    if (code && evsm_k > 0) e = evsm_patch((const BYTE *)code, len, evsm_k, &el);   /* ShadowNoiseFix, evsm.h */
    if (code && tex_orig) p = mip_shader_patch(e ? e : (const BYTE *)code, e ? el : len, &k);   /* MipFix, mipshader.h */
    if (!p) { p = e; e = 0; }
    free(e);
    if (p) {
        r = ps_orig(dev, p, *(DWORD *)(p + 24), cl, ps); free(p);
        if (SUCCEEDED(r)) { InterlockedIncrement(&ps_mip); return r; }
        InterlockedIncrement(&ps_mip_failed);                                     /* the original shader then */
    }
    if (aoblur_on && code && len == *(DWORD *)(aocp + 24) && !memcmp(code, aocp, len)) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : 0;
        ID3DBlob *b = 0, *err = 0; r = E_FAIL;
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
        if (aoblur_on || tex_orig || evsm_k > 0)
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

/* ---- TraceAnim (debug, off by default): finds the code advancing material animations (sea/cloud
   scrolling). Tamlib::CTmd2MatAnimePlayer's evaluate (140268a00, rcx = player, frame at +10h) is
   detoured; the first player whose frame moves gets a hardware write breakpoint (DR0), and the
   instructions writing it are logged with the frame step they applied. ---- */
static BYTE *trace_addr; static DWORD trace_tid; static float trace_prev;
static struct { void *player; float frame; } seen[64]; static int nseen, nwrites;
static ULONGLONG wrote_at[16]; static int nwrote;

static BYTE *watch[4]; static int nwatch;               /* up to 4 hardware write breakpoints */
static LONG CALLBACK trace_veh(EXCEPTION_POINTERS *e) {
    DWORD64 hit = e->ContextRecord->Dr6 & 15;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !hit) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    if (nwrites < 400) {
        ULONGLONG rip = (ULONGLONG)((BYTE *)e->ExceptionRecord->ExceptionAddress - base) + 0x140000000ULL;
        int w = hit & 1 ? 0 : hit & 2 ? 1 : hit & 4 ? 2 : 3, i;
        float now = *(float *)watch[w];
        for (i = 0; i < nwrote && wrote_at[i] != rip + w; i++);
        if (i == nwrote && nwrote < 16) {
            ULONGLONG *sp = (ULONGLONG *)e->ContextRecord->Rsp; int k, shown = 0;
            wrote_at[nwrote++] = rip + w;
            say("Trace: watch %d (%p) = %g, written before %llx; exe return addresses:", w, watch[w], now, rip);
            for (k = 0; k < 96 && shown < 4; k++)            /* game code on the stack (caller chain) */
                if (sp[k] > (ULONGLONG)base + 0x1000 && sp[k] < (ULONGLONG)base + 0x346000) {
                    say(" %llx", sp[k] - (ULONGLONG)base + 0x140000000ULL); shown++;
                }
            say("\n");
            save_log();
        }
        nwrites++;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

#include <tlhelp32.h>
static DWORD WINAPI set_watch(LPVOID p) {                  /* DR0 on every thread but this one */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE t; CONTEXT ctx;
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        if (!(t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID))) continue;
        SuspendThread(t);
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &ctx)) {
            DWORD64 *dr = &ctx.Dr0; int i;
            ctx.Dr7 = 0;
            for (i = 0; i < nwatch; i++) {                  /* Li enable, RW = write, LEN = 4 bytes */
                dr[i] = (DWORD64)watch[i]; ctx.Dr7 |= (1ULL << (2 * i)) | (1ULL << (16 + 4 * i)) | (3ULL << (18 + 4 * i));
            }
            SetThreadContext(t, &ctx);
        }
        ResumeThread(t); CloseHandle(t);
    }
    CloseHandle(snap);
    return 0;
}

/* per player: last frame and the step seen; after ~600 evaluations the steps are logged and a player
   stepping >= 0.9 per evaluation (60 fps speed at 120 fps) is watched, else the first moving one */
static float seen_step[64]; static int nevals;
static int trace_sea_on;
static void record(BYTE *player) {
    int i; float f = *(float *)(player + 0x10);
    if (trace_sea_on) return;
    if (!trace_addr) {
        for (i = 0; i < nseen && seen[i].player != player; i++);
        if (i == nseen && nseen < 64) { seen[nseen].player = player; seen[nseen++].frame = f; }
        else if (i < nseen) {
            if (f != seen[i].frame && f > seen[i].frame) seen_step[i] = f - seen[i].frame;
            seen[i].frame = f;
        }
        if (++nevals == 600) {
            int pick = -1;
            for (i = 0; i < nseen; i++) {
                say("TraceAnim: player %p step %g\n", seen[i].player, seen_step[i]);
                if (seen_step[i] >= 0.9f && pick < 0) pick = i;
            }
            for (i = 0; i < nseen && pick < 0; i++) if (seen_step[i] > 0) pick = i;
            if (pick >= 0) {
                trace_addr = watch[0] = (BYTE *)seen[pick].player + 0x10; nwatch = 1;
                say("TraceAnim: watching player %p\n", seen[pick].player);
                AddVectoredExceptionHandler(1, trace_veh);
                CloseHandle(CreateThread(0, 0, set_watch, 0, 0, 0));
            }
            save_log();
        }
    }
}

typedef void(__fastcall *Eval4)(BYTE *, void *, void *, void *);
static Eval4 mat_tramp, obj_tramp;
static BYTE *sea_player; static float sea_frame;
static void __fastcall mat_hook(BYTE *p, void *a, void *b, void *c) {
    float *mp = (float *)VA(0x140eeac30);                /* m_Param[1] in the global parameter block */
    float before = mp[1];
    record(p); mat_tramp(p, a, b, c);
    if (!sea_player && mp[1] != before && mp[1] != 0 && fabsf(mp[1] - 10 * mp[0]) < 1e-4f && fabsf(mp[3] - 2 * mp[1]) < 1e-4f) {
        sea_player = p; sea_frame = *(float *)(p + 0x10);
        say("TraceSea: sea material player %p, frame %g, param %g\n", p, sea_frame, mp[1]); save_log();
    } else if (p == sea_player && !nwatch) {
        float f = *(float *)(p + 0x10);
        if (f != sea_frame) {
            say("TraceSea: sea player frame step %g; watching it\n", f - sea_frame);
            watch[nwatch++] = p + 0x10; save_log();
            AddVectoredExceptionHandler(1, trace_veh);
            CloseHandle(CreateThread(0, 0, set_watch, 0, 0, 0));
        }
    }
}
static void __fastcall obj_hook(BYTE *p, void *a, void *b, void *c) { record(p); obj_tramp(p, a, b, c); }

static int detour(ULONGLONG va, const char *orig, int n, void *hook, Eval4 *tramp) {   /* n whole instructions */
    BYTE *t;
    if (!is(va, orig, n) || !(t = VirtualAlloc(0, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))) return 0;
    memcpy(t, VA(va), n);
    t[n] = 0xff; t[n + 1] = 0x25; *(DWORD *)(t + n + 2) = 0; *(BYTE **)(t + n + 6) = VA(va + n);
    *tramp = (Eval4)t;
    return jmp_at(va, hook);
}

static int trace_anim(void) {                       /* material player, object player evaluate */
    return detour(0x140268a00, "\x40\x55\x41\x54\x48\x83\xec\x48", 8, mat_hook, &mat_tramp) &&
           detour(0x1402685d0, "\x40\x53\x48\x81\xec\xb0\x00\x00\x00", 9, obj_hook, &obj_tramp);
}

/* TraceMotion (debug): character motions advance by frame += dt * speed (Tamlib::CTmoInstance and
   CBlendTmoInstance update, dt in xmm1). Normal callers pass the frame-time step (~0.5 at 120 fps);
   every call site seen passing exactly 1.0 (per-frame stepping, too fast above 60 fps) is logged once
   with the motion's frame and length. */
typedef void(__fastcall *UpdFn)(BYTE *, float);
static UpdFn tmo_tramp, blend_tramp;
static ULONGLONG mot_seen[512]; static int nmot;
static void mot_record(BYTE *m, float dt, ULONGLONG *sp) {
    ULONGLONG chain[6], h = 0; int i, n = 0;
    if (dt != 1.0f || *(float *)(base + 0x60fc00) >= 0.75f) return;     /* only at high frame rates */
    if (*(float *)(m + 0x50) <= 0) return;                              /* empty motion */
    for (i = 0; i < 160 && n < 6; i++)                                   /* game code on the stack */
        if (sp[i] > (ULONGLONG)base + 0x1000 && sp[i] < (ULONGLONG)base + 0x346000) {
            chain[n++] = sp[i] - (ULONGLONG)base + 0x140000000ULL; h = h * 31 + chain[n - 1];
        }
    if (n && chain[0] == 0x1402239bbULL) return;                         /* UI animations (separate issue) */
    for (i = 0; i < nmot && mot_seen[i] != h; i++);
    if (i < nmot || nmot == 512) return;
    mot_seen[nmot++] = h;
    say("TraceMotion: motion %p advanced by a fixed 1.0 per frame (frame %g of %g, speed %g); chain:",
        m, *(float *)(m + 0x1c), *(float *)(m + 0x50), *(float *)(m + 0x20));
    for (i = 0; i < n; i++) say(" %llx", chain[i]);
    say("\n");
    save_log();
}
static void __fastcall tmo_hook(BYTE *m, float dt) { mot_record(m, dt, (ULONGLONG *)_AddressOfReturnAddress()); tmo_tramp(m, dt); }
static void __fastcall blend_hook(BYTE *m, float dt) { mot_record(m, dt, (ULONGLONG *)_AddressOfReturnAddress()); blend_tramp(m, dt); }

static int trace_motion(void) {
    return detour(0x14025cf60, "\x40\x53\x48\x83\xec\x40", 6, tmo_hook, (Eval4 *)&tmo_tramp) &&
           detour(0x1400428f0, "\x40\x53\x48\x81\xec\x80\x00\x00\x00", 9, blend_hook, (Eval4 *)&blend_tramp);
}

/* TraceSea (debug): 45 s after start, scans the heap for the sea material's parameters
   (m_Param[0] = 100, .5, .5, 0; m_Param[1] = x, 10x, -x, 20x, seen in RenderDoc) and watches
   m_Param[1].y with the hardware breakpoint, logging the code that scrolls it. */
static float sea_value(void) {                            /* newest copy of the sea scroll, 0 if none */
    MEMORY_BASIC_INFORMATION mi; BYTE *a = 0; float best = 0;
    while (VirtualQuery(a, &mi, sizeof mi) == sizeof mi) {
        if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && (mi.Protect & PAGE_READWRITE) && !(mi.Protect & PAGE_GUARD)) {
            float *f = (float *)mi.BaseAddress, *e = (float *)((BYTE *)mi.BaseAddress + mi.RegionSize) - 8;
            for (; f < e; f++)
                if (f[0] == 100.0f && f[1] == 0.5f && f[2] == 0.5f && f[3] == 0.0f && f[5] != 0 &&
                    fabsf(f[5] - 10 * f[4]) < 1e-4f && fabsf(f[6] + f[4]) < 1e-5f && fabsf(f[7] - 2 * f[5]) < 1e-4f &&
                    (best == 0 || f[5] < best)) best = f[5];            /* it decreases: newest = smallest */
        }
        a = (BYTE *)mi.BaseAddress + mi.RegionSize;
    }
    return best;
}

static DWORD WINAPI sea_scan(LPVOID p) {
    MEMORY_BASIC_INFORMATION mi; BYTE *a = 0; int found = 0;
    LARGE_INTEGER t0, t1, fq; float s0, s1;
    Sleep(45000);
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0); s0 = sea_value();
    Sleep(10000);
    QueryPerformanceCounter(&t1); s1 = sea_value();
    say("TraceSea: scroll %g -> %g in %.2f s = %g per second\n", s0, s1, (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart,
        (s1 - s0) / ((double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart));
    save_log();
    {   /* the material parameter queue (count at 140d0ff90, then {int index; float value}[32]) */
        int *q = (int *)VA(0x140d0ff90), tries, k;
        int logged = 0;
        for (tries = 0; tries < 200000 && !nwatch; tries++, (tries & 63) ? 0 : Sleep(0))
            for (k = 0; k < q[0] && k < 32; k++) {
                int *en = q + 1 + 2 * k; float v = *(float *)(en + 1);
                if (en[0] >= 4 && en[0] <= 7 && logged < 24) { say("TraceSea: queue[%d] = param %d: %g\n", k, en[0], v); logged++; }
                if (en[0] == 5 && v < -0.3f && v > -3.0f) {
                    watch[nwatch++] = (BYTE *)(en + 1);
                    say("TraceSea: watching queue slot %d (param 5 = %g)\n", k, v);
                    break;
                }
            }
        save_log();
        if (nwatch) { AddVectoredExceptionHandler(1, trace_veh); set_watch(0); }
    }
    return 0;
    while (VirtualQuery(a, &mi, sizeof mi) == sizeof mi) {
        if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mi.Protect & PAGE_GUARD)) {
            float *f = (float *)mi.BaseAddress, *e = (float *)((BYTE *)mi.BaseAddress + mi.RegionSize) - 8;
            for (; f < e; f++)
                if (f[0] == 100.0f && f[1] == 0.5f && f[2] == 0.5f && f[3] == 0.0f && f[5] != 0 &&
                    fabsf(f[5] - 10 * f[4]) < 1e-4f && fabsf(f[6] + f[4]) < 1e-5f && fabsf(f[7] - 2 * f[5]) < 1e-4f) {
                    say("TraceSea: sea params at %p: %g %g %g %g\n", f + 4, f[4], f[5], f[6], f[7]);
                    if (nwatch < 4) watch[nwatch++] = (BYTE *)(f + 5);
                    found++;
                }
        }
        a = (BYTE *)mi.BaseAddress + mi.RegionSize;
    }
    say("TraceSea: scan done, %d found, watching %d\n", found, nwatch); save_log();
    if (nwatch) { AddVectoredExceptionHandler(1, trace_veh); set_watch(0); }
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
    if (opt("TraceAnim", 0)) say("TraceAnim: %s\n", trace_anim() ? "on (debug)" : "SKIPPED - unexpected exe bytes");
    if ((trace_sea_on = opt("TraceSea", 0))) { trace_anim(); CloseHandle(CreateThread(0, 0, sea_scan, 0, 0, 0)); }
    if (opt("TraceMotion", 0)) say("TraceMotion: %s\n", trace_motion() ? "on (debug)" : "SKIPPED - unexpected exe bytes");
    v = opt("SkipIntro", 2);
    if (v >= 0 && v <= 2) report("SkipIntro", v, fix_intro(v)); else say("SkipIntro=%d: SKIPPED - must be 0..2\n", v);
    v = opt("ShadowNoiseFix", 5);
    if (v >= 0 && v <= 9) { evsm_k = v ? powf(10.0f, -(float)v) : 0; say("ShadowNoiseFix=%d: %s\n", v, v ? "on" : "off"); }
    else say("ShadowNoiseFix=%d: SKIPPED - must be 0..9\n", v);
    v = opt("AOBlur", 1);
    aoblur_on = v && !memcmp(VA(0x1404d28e0), "DXBC", 4) && *(DWORD *)VA(0x1404d28f8) < 4096;
    say("AOBlur=%d: %s\n", v, !v ? "off" : aoblur_on ? "on" : "SKIPPED - unexpected exe bytes (other game version?)");
    GetPrivateProfileStringA("SKMod", "NearClip", "0", s, sizeof s, ini);
    {
        float n = (float)atof(s);
        if (n <= 1) say("NearClip=%s: off\n", s);
        else if (n > 50) say("NearClip=%s: SKIPPED - must be 0 or 2..50\n", s);
        else say("NearClip=%.0f: %s\n", n, fix_near(n) ? "on (near grows with zoom, up to this factor)" : "SKIPPED - unexpected exe bytes (other game version?)");
    }
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
