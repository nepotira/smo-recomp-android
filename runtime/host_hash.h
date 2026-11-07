/* host_hash.h - SHA-1 and MD5 for the nn::crypto answers on hosts without a system hash API (Android port).
 * Straight from the definitions (FIPS 180-4 section 6.1, RFC 1321); nnmisc.c's host_hash_selftest compares them with the
 * published test vectors ("abc", the empty string, 1,000,000 'a'). Header-only: one user (nnmisc.c). */
#ifndef HOST_HASH_H
#define HOST_HASH_H
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; } HostSha1;
typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; } HostMd5;

static inline uint32_t hh_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(HostSha1 *s, const uint8_t *p) {
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = hh_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = hh_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = hh_rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(HostSha1 *s) {
    static const uint32_t iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
}
static void sha1_update(HostSha1 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) { s->buf[s->len++ % 64] = *p++; if (s->len % 64 == 0) sha1_block(s, s->buf); }
}
static void sha1_final(HostSha1 *s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t one = 0x80, zero = 0;
    sha1_update(s, &one, 1);
    while (s->len % 64 != 56) sha1_update(s, &zero, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); sha1_update(s, &b, 1); }
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

static void md5_block(HostMd5 *s, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    for (int i = 0; i < 16; i++) m[i] = p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t t = d;
        d = c; c = b;
        b = b + hh_rol(a + f + K[i] + m[g], R[i]);
        a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
}
static void md5_init(HostMd5 *s) {
    static const uint32_t iv[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
}
static void md5_update(HostMd5 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) { s->buf[s->len++ % 64] = *p++; if (s->len % 64 == 0) md5_block(s, s->buf); }
}
static void md5_final(HostMd5 *s, uint8_t out[16]) {
    uint64_t bits = s->len * 8;
    uint8_t one = 0x80, zero = 0;
    md5_update(s, &one, 1);
    while (s->len % 64 != 56) md5_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) { uint8_t b = (uint8_t)(bits >> (8 * i)); md5_update(s, &b, 1); }
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(s->h[i / 4] >> (8 * (i % 4)));
}
#endif
