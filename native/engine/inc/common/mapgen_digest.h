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
==============================================================================

MAPGEN-1 - CRC32 and SHA-256 for the snapshot container

Contract section 8 requires a CRC32 per stored chunk and a SHA-256 over the
canonical payload, plus SHA-256 over every training source.

--- Why these are here and not borrowed --------------------------------------

Two implementations already exist in this tree, and neither is usable:

  * `MapGenIpc_Crc32` (`src/mapgen/mapgen_ipc.c:37-60`) builds its lookup table
    lazily into file-scope `static` state. That is a data race the moment two
    Training threads hash at once, and the whole point of the M2 modules is
    that they hold none. The version here is stateless - no table, no lazy
    init - and the guard proves the two agree on random input, which turns the
    duplication into a cross-check;

  * `Q2PROX_CfgSha256Raw` (`src/common/q2prox_config_ownership.c:1184`) is
    correct, but it lives inside an accepted engine module that the headless
    worker must not drag in. Extracting it would mean editing shipped code for
    the convenience of a new one.

The SHA-256 here is checked against the FIPS 180-4 published vectors AND
against Python's `hashlib` over thousands of random inputs, so it does not rest
on agreement with anything in this repository.

==============================================================================
*/

#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAPGEN_SHA256_BYTES  32
#define MAPGEN_SHA256_HEX    65     /* 64 digits plus the terminator */

/*
 * CRC32, the IEEE 802.3 polynomial in its reflected form (0xEDB88320), which
 * is what zlib, PNG and the IPC frame header all use.
 *
 * Stateless by construction: no table, no initialisation, safe to call from
 * any number of threads at once.
 */
uint32_t MapGenDigest_Crc32(const void *data, size_t size);

/* Incremental form, for hashing something that is not contiguous. Start with
   `MapGenDigest_Crc32Init()`. */
uint32_t MapGenDigest_Crc32Init(void);
uint32_t MapGenDigest_Crc32Update(uint32_t state, const void *data, size_t size);
uint32_t MapGenDigest_Crc32Final(uint32_t state);

/* SHA-256, FIPS 180-4. */
typedef struct {
    uint32_t      h[8];
    uint64_t      bits;
    unsigned char buffer[64];
    size_t        pending;
} mapgen_sha256_t;

void MapGenDigest_Sha256Init(mapgen_sha256_t *ctx);
void MapGenDigest_Sha256Update(mapgen_sha256_t *ctx, const void *data, size_t size);
void MapGenDigest_Sha256Final(mapgen_sha256_t *ctx, uint8_t out[MAPGEN_SHA256_BYTES]);

/* One-shot. */
void MapGenDigest_Sha256(const void *data, size_t size, uint8_t out[MAPGEN_SHA256_BYTES]);

/* Lowercase hex, always NUL-terminated. */
void MapGenDigest_Sha256Hex(const uint8_t digest[MAPGEN_SHA256_BYTES],
                            char out[MAPGEN_SHA256_HEX]);
