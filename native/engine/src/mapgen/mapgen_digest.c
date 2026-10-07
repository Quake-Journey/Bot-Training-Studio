/*
Copyright (C) 2026 Q2PRO-X

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

/*
 * MAPGEN-1 - CRC32 and SHA-256.
 *
 * Both are stateless with respect to the module: nothing is cached, nothing is
 * initialised on first use, and two threads may hash at once. The CRC is
 * computed without a lookup table for exactly that reason - a table that is
 * filled lazily is shared mutable state, and this module is not allowed any.
 */

#include "common/mapgen_digest.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* CRC32, reflected IEEE 802.3 polynomial                                    */

#define CRC32_POLY  0xEDB88320u

uint32_t MapGenDigest_Crc32Init(void)
{
    return 0xFFFFFFFFu;
}

uint32_t MapGenDigest_Crc32Update(uint32_t state, const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    if (!p)
        return state;
    for (size_t i = 0; i < size; i++) {
        state ^= p[i];
        /* Eight unrolled steps rather than a table: the answer is identical
           and there is no state to share. */
        for (int bit = 0; bit < 8; bit++)
            state = (state >> 1) ^ (CRC32_POLY & (uint32_t)(-(int32_t)(state & 1u)));
    }
    return state;
}

uint32_t MapGenDigest_Crc32Final(uint32_t state)
{
    return state ^ 0xFFFFFFFFu;
}

uint32_t MapGenDigest_Crc32(const void *data, size_t size)
{
    return MapGenDigest_Crc32Final(
        MapGenDigest_Crc32Update(MapGenDigest_Crc32Init(), data, size));
}

/* ------------------------------------------------------------------------ */
/* SHA-256, FIPS 180-4                                                       */

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(mapgen_sha256_t *ctx, const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
        const uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void MapGenDigest_Sha256Init(mapgen_sha256_t *ctx)
{
    if (!ctx)
        return;
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->bits = 0;
    ctx->pending = 0;
}

void MapGenDigest_Sha256Update(mapgen_sha256_t *ctx, const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    if (!ctx || !p)
        return;

    ctx->bits += (uint64_t)size * 8u;

    if (ctx->pending) {
        const size_t room = 64 - ctx->pending;
        const size_t take = size < room ? size : room;
        memcpy(ctx->buffer + ctx->pending, p, take);
        ctx->pending += take;
        p += take;
        size -= take;
        if (ctx->pending == 64) {
            sha256_block(ctx, ctx->buffer);
            ctx->pending = 0;
        }
    }
    while (size >= 64) {
        sha256_block(ctx, p);
        p += 64;
        size -= 64;
    }
    if (size) {
        memcpy(ctx->buffer, p, size);
        ctx->pending = size;
    }
}

void MapGenDigest_Sha256Final(mapgen_sha256_t *ctx, uint8_t out[MAPGEN_SHA256_BYTES])
{
    if (!ctx || !out)
        return;

    const uint64_t bits = ctx->bits;

    /*
     * The pad length is computed, not looped towards. An earlier version fed
     * zero bytes back through Update until `pending` reached 56, which is
     * correct while `pending` is correct and an infinite loop the moment it is
     * not - a controlled-RED mutation to the buffering path hung the whole
     * suite rather than failing it. A `while` whose exit depends on internal
     * state it does not own has no business being here.
     */
    unsigned char pad[128];
    pad[0] = 0x80;
    const size_t after = (ctx->pending + 1) % 64;
    const size_t zeros = after <= 56 ? 56 - after : 120 - after;
    memset(pad + 1, 0, zeros);
    MapGenDigest_Sha256Update(ctx, pad, 1 + zeros);
    ctx->bits = bits;                    /* padding is not message length */

    unsigned char length[8];
    for (int i = 0; i < 8; i++)
        length[i] = (unsigned char)((bits >> (56 - i * 8)) & 0xFFu);
    memcpy(ctx->buffer + 56, length, 8);
    sha256_block(ctx, ctx->buffer);
    ctx->pending = 0;

    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)((ctx->h[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = (uint8_t)((ctx->h[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = (uint8_t)((ctx->h[i] >> 8) & 0xFFu);
        out[i * 4 + 3] = (uint8_t)(ctx->h[i] & 0xFFu);
    }
}

void MapGenDigest_Sha256(const void *data, size_t size, uint8_t out[MAPGEN_SHA256_BYTES])
{
    mapgen_sha256_t ctx;
    MapGenDigest_Sha256Init(&ctx);
    MapGenDigest_Sha256Update(&ctx, data, size);
    MapGenDigest_Sha256Final(&ctx, out);
}

void MapGenDigest_Sha256Hex(const uint8_t digest[MAPGEN_SHA256_BYTES],
                            char out[MAPGEN_SHA256_HEX])
{
    static const char HEX[] = "0123456789abcdef";
    if (!out)
        return;
    if (!digest) {
        out[0] = '\0';
        return;
    }
    for (int i = 0; i < MAPGEN_SHA256_BYTES; i++) {
        out[i * 2 + 0] = HEX[(digest[i] >> 4) & 0xFu];
        out[i * 2 + 1] = HEX[digest[i] & 0xFu];
    }
    out[MAPGEN_SHA256_BYTES * 2] = '\0';
}
