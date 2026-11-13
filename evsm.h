/* ShadowNoiseFix: the shadow receivers use exponential variance shadow maps. The variance is
   E[x^2] - E[x]^2 of exp-warped depth in 32-bit float, two huge nearly equal numbers, and the shader
   never clamps it: near occluders it comes out as noise or negative, and the shadow there turns into
   flickering speckles (e.g. the band where a rock meets the sand). After every
       mad rD.y, -rM.x, rM.x, rM.y            (variance)
   this inserts, using one extra temp register rN:
       mul rN.x, rM.x, rM.x / mul rN.x, rN.x, k / max rD.y, rD.y, rN.x
   i.e. variance >= k * mean^2. DXBC control flow is structured (no jump offsets), so inserting is safe;
   chunk sizes/offsets and the checksum are updated. Shared by skmod.c and test_evsm.c. */
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

#define EVSM_INS 21                                   /* dwords inserted per variance */

/* Returns a malloc'd patched copy (size in *out_len) or 0 when the shader is not one to patch. */
static BYTE *evsm_patch(const BYTE *blob, SIZE_T len, float k, DWORD *out_len) {
    DWORD st[4], n, c, i, size, shex = 0, slen, *t, nt, temps_at = 0, at[16], cnt = 0, delta, kbits;
    BYTE *out, *p;
    if (len < 32 || memcmp(blob, "DXBC", 4) || (size = *(DWORD *)(blob + 24)) > len || size < 32) return 0;
    if (!has_bytes(blob, size, "shadowFieldMap")) return 0;
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
    t = (DWORD *)(blob + shex + 8); nt = slen / 4;
    if ((t[0] >> 16) != 0 || t[1] != nt) return 0;              /* program type 0 = pixel shader */
    for (i = 2; i < nt;) {
        DWORD op = t[i] & 0x7ff, ln = t[i] >> 24 & 0x7f;
        if (op == 0x35) ln = i + 1 < nt ? t[i + 1] : 0;           /* customdata */
        if (!ln || i + ln > nt) return 0;
        if (op == 0x68 && ln == 2) temps_at = i + 1;               /* dcl_temps */
        if (ln == 10 && t[i] == 0x0a000032 && t[i + 1] == 0x00100022 && t[i + 3] == 0x8010000a && t[i + 4] == 0x41 &&
            t[i + 6] == 0x0010000a && t[i + 8] == 0x0010001a && t[i + 5] == t[i + 7] && t[i + 7] == t[i + 9]) {
            if (cnt == 16) return 0;
            at[cnt++] = i + ln;                                    /* insert after it */
        }
        i += ln;
    }
    if (!cnt || !temps_at) return 0;
    delta = cnt * EVSM_INS * 4;
    if (!(out = (BYTE *)malloc(size + delta))) return 0;
    memcpy(&kbits, &k, 4);
    /* header + chunks up to the SHEX tokens, then tokens with insertions, then the rest */
    p = out;
    memcpy(p, blob, shex + 8); p += shex + 8;
    for (i = 0, c = 0; i < nt; i++) {
        DWORD v = t[i];
        if (i == temps_at) v++;
        if (i == 1) v += cnt * EVSM_INS;
        *(DWORD *)p = v; p += 4;
        if (c < cnt && i + 1 == at[c]) {
            DWORD N = t[temps_at], M = t[at[c] - 5], D = t[at[c] - 8];
            DWORD ins[EVSM_INS] = {0x07000038, 0x00100012, N, 0x0010000a, M, 0x0010000a, M,
                                   0x07000038, 0x00100012, N, 0x0010000a, N, 0x00004001, kbits,
                                   0x07000034, 0x00100022, D, 0x0010001a, D, 0x0010000a, N};
            memcpy(p, ins, sizeof ins); p += sizeof ins; c++;
        }
    }
    memcpy(p, blob + shex + 8 + slen, size - (shex + 8 + slen));
    *(DWORD *)(out + shex + 4) += delta;
    *(DWORD *)(out + 24) += delta;
    for (i = 0; i < n; i++) if (*(DWORD *)(out + 32 + 4 * i) > shex) *(DWORD *)(out + 32 + 4 * i) += delta;
    dxbc_checksum(out, size + delta, (DWORD *)(out + 4));
    *out_len = size + delta;
    return out;
}
