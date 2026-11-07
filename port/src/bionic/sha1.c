// SHA1Init/Update/Final, exported by bionic's libc and imported by libunity.
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t state[5];
    uint64_t count; // bits
    uint8_t buffer[64];
} SHA1_CTX;

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 |
               block[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void b_SHA1Init(SHA1_CTX *c)
{
    static const uint32_t init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(c->state, init, sizeof init);
    c->count = 0;
}

void b_SHA1Update(SHA1_CTX *c, const uint8_t *data, uint32_t len)
{
    size_t fill = (size_t)((c->count >> 3) & 63);
    c->count += (uint64_t)len << 3;
    while (len) {
        size_t n = 64 - fill;
        if (n > len)
            n = len;
        memcpy(c->buffer + fill, data, n);
        fill += n;
        data += n;
        len -= (uint32_t)n;
        if (fill == 64) {
            transform(c->state, c->buffer);
            fill = 0;
        }
    }
}

void b_SHA1Final(uint8_t digest[20], SHA1_CTX *c)
{
    uint64_t bits = c->count;
    uint8_t pad = 0x80, zero = 0, len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    b_SHA1Update(c, &pad, 1);
    while (((c->count >> 3) & 63) != 56)
        b_SHA1Update(c, &zero, 1);
    b_SHA1Update(c, len, 8);
    for (int i = 0; i < 20; i++)
        digest[i] = (uint8_t)(c->state[i / 4] >> (24 - 8 * (i % 4)));
}
