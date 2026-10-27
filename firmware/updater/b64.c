#include "b64.h"

static int b64_val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

long b64_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap)
{
    size_t i, o = 0;
    size_t pad = 0;
    int quad[4];
    size_t q;

    if (inlen == 0 || (inlen % 4) != 0)
        return -1;

    for (i = 0; i < inlen; i += 4) {
        q = 0;
        for (q = 0; q < 4; q++) {
            unsigned char c = (unsigned char)in[i + q];
            if (c == '=') {
                /* '=' only in the last quad, only in the last two positions. */
                if (i + 4 != inlen || q < 2)
                    return -1;
                quad[q] = 0;
                pad++;
            } else {
                int v = b64_val(c);
                if (v < 0 || pad)
                    return -1;
                quad[q] = v;
            }
        }
        if (o + (pad == 2 ? 1 : (pad == 1 ? 2 : 3)) > outcap)
            return -1;
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        if (pad < 2)
            out[o++] = (unsigned char)(((quad[1] & 0x0f) << 4) | (quad[2] >> 2));
        if (pad < 1)
            out[o++] = (unsigned char)(((quad[2] & 0x03) << 6) | quad[3]);
    }
    return (long)o;
}
