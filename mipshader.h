/* MipFix, shader part: the material pixel shaders (field, objects, characters) read their textures with
   sample_l ... l(0) - always the full-size mip, probably carried over from the Vita version - so no
   mipmap is ever used and distant detail (sand glitter map, planks) turns into flickering speckles.
   Top-level reads (outside if/loop/switch, where derivatives are defined) of material shaders are
   changed to sample_b ... 0: same operands, hardware mip selection with bias 0. The DXBC checksum is
   recomputed. Shared by skmod.c and test_mipshader.c. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

static DWORD md5_k[64];
static const BYTE md5_s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(DWORD st[4], const BYTE *blk) {
    const DWORD *m = (const DWORD *)blk; DWORD a = st[0], b = st[1], c = st[2], d = st[3], f, t; int i, g;
    if (!md5_k[0]) for (i = 0; i < 64; i++) md5_k[i] = (DWORD)(fabs(sin(i + 1.0)) * 4294967296.0);
    for (i = 0; i < 64; i++) {
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        f += a + md5_k[i] + m[g];
        a = d; d = c; c = b;
        t = md5_s[i]; b += (f << t) | (f >> (32 - t));
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

/* DXBC checksum: MD5 compression over blob[20..] with DXBC's own padding */
static void dxbc_checksum(const BYTE *blob, DWORD size, DWORD st[4]) {
    const BYTE *data = blob + 20; DWORD n = size - 20, bits = n * 8, full = n - n % 64, rest = n % 64, i;
    BYTE blk[64];
    st[0] = 0x67452301; st[1] = 0xefcdab89; st[2] = 0x98badcfe; st[3] = 0x10325476;
    for (i = 0; i < full; i += 64) md5_block(st, data + i);
    memset(blk, 0, 64);
    if (rest >= 56) {
        memcpy(blk, data + full, rest); blk[rest] = 0x80; md5_block(st, blk);
        memset(blk, 0, 64); *(DWORD *)blk = bits;
    } else {
        *(DWORD *)blk = bits; memcpy(blk + 4, data + full, rest); blk[4 + rest] = 0x80;
    }
    *(DWORD *)(blk + 60) = (bits >> 2) | 1;
    md5_block(st, blk);
}

static int has_bytes(const BYTE *p, DWORD n, const char *s) {
    DWORD l = (DWORD)strlen(s), i;
    for (i = 0; i + l <= n; i++) if (!memcmp(p + i, s, l)) return 1;
    return 0;
}

/* Returns a malloc'd patched copy (same size) or 0 when there is nothing to change. *count = reads changed. */
static BYTE *mip_shader_patch(const BYTE *blob, SIZE_T len, int *count) {
    DWORD st[4], n, c, i, size, shex = 0, slen, *t, nt; int depth = 0;
    BYTE *out;
    *count = 0;
    if (len < 32 || memcmp(blob, "DXBC", 4) || (size = *(DWORD *)(blob + 24)) > len || size < 32) return 0;
    if (!has_bytes(blob, size, "colorMap")) return 0;          /* material shaders only */
    dxbc_checksum(blob, size, st);
    if (memcmp(st, blob + 4, 16)) return 0;
    n = *(DWORD *)(blob + 28);
    if (n > 64 || 32 + 4 * n > size) return 0;
    for (i = 0; i < n; i++) {
        c = *(DWORD *)(blob + 32 + 4 * i);
        if (c > size - 8) return 0;
        if (!memcmp(blob + c, "SHEX", 4) || !memcmp(blob + c, "SHDR", 4)) shex = c;
    }
    if (!shex || (slen = *(DWORD *)(blob + shex + 4)) > size - shex - 8 || slen < 8) return 0;
    if (!(out = (BYTE *)malloc(size))) return 0;
    memcpy(out, blob, size);
    t = (DWORD *)(out + shex + 8); nt = slen / 4;
    if ((t[0] >> 16) != 0 || t[1] != nt) { free(out); return 0; }  /* pixel shaders only */
    for (i = 2; i < nt;) {
        DWORD op = t[i] & 0x7ff, ln = t[i] >> 24 & 0x7f;
        if (op == 0x35) ln = i + 1 < nt ? t[i + 1] : 0;              /* customdata */
        if (!ln || i + ln > nt) { free(out); return 0; }
        if (op == 0x1f || op == 0x30 || op == 0x4c) depth++;          /* if, loop, switch */
        else if (op == 0x15 || op == 0x16 || op == 0x17) depth--;     /* endif, endloop, endswitch */
        else if (op == 0x48 && !depth && !(t[i] & 0x80000000) && t[i + ln - 2] == 0x00004001 && t[i + ln - 1] == 0) {
            t[i] = (t[i] & ~0x7ffu) | 0x4a;                           /* sample_l, lod 0 -> sample_b, bias 0 */
            (*count)++;
        }
        i += ln;
    }
    if (!*count) { free(out); return 0; }
    dxbc_checksum(out, size, (DWORD *)(out + 4));
    return out;
}
