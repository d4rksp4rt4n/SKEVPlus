/* ShadowNoiseFix: the shadow receivers use exponential variance shadow maps. The variance is
   E[x^2] - E[x]^2 of exp-warped depth in 32-bit float (two huge nearly equal numbers, e.g. 1.92e14) and
   the shader never clamps it. Where it cancels to slightly below zero, log() of the shadow term gives
   NaN, and the GPU's min(NaN, 1) = 1 makes that pixel fully lit: bright speckles inside shadows that
   flicker as the camera moves (seen with a RenderDoc shader debug of the sand). After every
       mad rD.c, -rM.x, rM.x, rM.y            (variance; c = any one component)
   this inserts, using one extra temp register rN:
       mul rN.x, rM.x, rM.x / mul rN.x, rN.x, k / max rD.c, rD.c, rN.x
   i.e. variance >= k * mean^2. DXBC control flow is structured (no jump offsets), so inserting is safe;
   chunk sizes/offsets and the checksum are updated. Needs mipshader.h (DXBC checksum).
   Shared by skmod.c and test_mipshader.c. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
        if (ln == 10 && t[i] == 0x0a000032 && (t[i + 1] == 0x00100012 || t[i + 1] == 0x00100022 || t[i + 1] == 0x00100042 ||
            t[i + 1] == 0x00100082) && t[i + 3] == 0x8010000a && t[i + 4] == 0x41 &&
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
            DWORD N = t[temps_at], M = t[at[c] - 5], D = t[at[c] - 8], dm = t[at[c] - 9];
            DWORD dc = dm == 0x00100012 ? 0 : dm == 0x00100022 ? 1 : dm == 0x00100042 ? 2 : 3;   /* written component */
            DWORD ins[EVSM_INS] = {0x07000038, 0x00100012, N, 0x0010000a, M, 0x0010000a, M,
                                   0x07000038, 0x00100012, N, 0x0010000a, N, 0x00004001, kbits,
                                   0x07000034, dm, D, 0x0010000a | dc << 4, D, 0x0010000a, N};
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
