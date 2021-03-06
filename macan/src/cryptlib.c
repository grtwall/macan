/*
 *  Copyright 2014, 2015 Czech Technical University in Prague
 *
 *  Authors: Michal Sojka <sojkam1@fel.cvut.cz>
 *           Radek Matějka <radek.matejka@gmail.com>
 *           Ondřej Kulatý <kulatond@fel.cvut.cz>
 *
 *  This file is part of MaCAN.
 *
 *  MaCAN is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  MaCAN is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with MaCAN.	If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "cryptlib.h"
#include "endian.h"
#include "macan_private.h"

#ifdef WITH_KLEE
#include "klee.h"
#endif

/* ToDo:
 * 	consider endianness
 */

/**
 * aes_wrap() - AES key wrap algorithm
 * @kye:     AES key
 * @length:  length of src
 * @dst:     cipher text will be written to, i.e. (length + 8) bytes
 * @src:     plain text
 *
 * aes_wrap() ciphers data at src to produce cipher text at dst. It is
 * implemented as specified in RFC 3394.
 */
void macan_aes_wrap(const struct macan_key *key, size_t length, uint8_t *dst, const uint8_t *src)
{
	uint8_t a[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
	uint8_t b[16];
	size_t block_size = 16;
	size_t t, n;
	uint32_t *af;
	unsigned i, j;

	assert((length % 8) == 0);

	memcpy(dst + 8, src, length);
	n = length / 8;

	for (j = 0; j < 6; j++) {
		for (i = 1; i < n + 1; i++) {
			memcpy(b, a, 8);
			memcpy(b + 8, dst + (8 * i), 8);
			macan_aes_encrypt(key, (unsigned)block_size, b, b);

			memcpy(a, b, 8);
			t = (n*j) + i;
			af = (uint32_t *)(a + 4);
			*af = *af ^ htobe32((unsigned)t);

			memcpy(dst + (8 * i), b + 8, 8);   /* ToDo: write to dst */
		}
	}

	memcpy(dst, a, 8);
}

/**
 * aes_unwrap() - AES key unwrap algorithm
 * @key:     AES key
 * @length:  length of src in bytes
 * @dst:     plain text will be written to, i.e. (length - 8) bytes
 * @src:     cipher text
 * @tmp:     auxiliary buffer with length size,
 *
 * The function unwraps key data stored at src. An auxiliary buffer tmp is
 * required, although it is possible to supply src as tmp.
 */
int macan_aes_unwrap(const struct macan_key *key, size_t length, uint8_t *dst, uint8_t *src, uint8_t *tmp)
{
	uint8_t iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };
	uint8_t b[16];
	size_t block_size = 16;
	uint32_t t, n;
	unsigned i;
	int j,k;

	assert((length % 8) == 0 && length > 0);

	memmove(tmp, src, length);
	n = (uint32_t)length / 8 - 1;

	for (j = 5; j >= 0; j--) {
		for (i = n; i > 0; i--) {
			t = (n*(uint32_t)j) + i;
			t = htobe32(t);
			/* We must do bytewise XOR because operands might not be aligned naturally,
			 * we got traps on Tricore.
			 */
			for (k = 0; k < 4; k++) {
				*(tmp + 4 + k) = *(tmp + 4 + k) ^ *(((uint8_t *)&t) + k);
			}

			memcpy(b, tmp, 8);
			memcpy(b + 8, tmp + (8 * i), 8);
			macan_aes_decrypt(key, (unsigned)block_size, b, b);

			memcpy(tmp, b, 8);
			memcpy(tmp + (8 * i), b + 8, 8);   /* ToDo: write to dst */
		}
	}

	if (!memchk(tmp, iv, 8)) {
		memset(dst, 0, length - 8);
		return 1;
	} else {
		memcpy(dst, tmp + 8, length - 8);
		return 0;
	}
}

/**
 * checks a message authenticity
 *
 * The function computes CMAC of the given plain text and compares
 * it against cmac4. Returns 1 if CMACs matches.
 *
 * @param skey  128-bit session key
 * @param cmac4: points to CMAC message part, i.e. 4 bytes CMAC
 * @param plain: plain text to be CMACked and checked against
 * @param len:   length of plain text in bytes
 */
int macan_check_cmac(struct macan_ctx *ctx, struct macan_key *skey, const uint8_t *cmac4,
		     uint8_t *plain, int time_index, unsigned len)
{
	uint8_t cmac[16];
	uint32_t *time_ptr;

	if (time_index < 0 || (unsigned)time_index > len - sizeof(*time_ptr)) {
		macan_aes_cmac(skey, len, cmac, plain);
		/* add memcmp instead of memchk */
		return memchk(cmac4, cmac, 4);
	}

	uint32_t time = (uint32_t)macan_get_time(ctx);
	int delta_t;
	time_ptr = (uint32_t *)&plain[time_index];

	for (delta_t = -1; delta_t <= 1; delta_t++) {
		*time_ptr = htole32(time + (uint32_t)delta_t);
		macan_aes_cmac(skey, len, cmac, plain);

		if (memcmp(cmac4, cmac, 4) == 0) {
			return 1;
		}
	}

	return 0;
}

/**
 * sign() - signs a message with CMAC
 * @skey:  128-bit session key
 * @cmac4: 4 bytes of the CMAC signature will be written to
 * @plain: a plain text to sign
 * @len:   length of the plain text
 */
void macan_sign(struct macan_key *skey, uint8_t *cmac4, uint8_t *plain, unsigned len)
{
	uint8_t cmac[16];
	macan_aes_cmac(skey, len, cmac, plain);
	memcpy(cmac4, cmac, 4);
}

/**
 * unwrap_key() - deciphers AES-WRAPed key
 */
void macan_unwrap_key(const struct macan_key *key, size_t srclen, uint8_t *dst, uint8_t *src)
{
#ifdef WITH_KLEE
	klee_make_symbolic(dst+16, 7, "aes unwrapped key");
	(void)key, (void)srclen, (void)src; //Fixes warnings.
#else
	macan_aes_unwrap(key, srclen, dst, src, src);
#endif
}
