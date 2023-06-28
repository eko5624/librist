/* librist. Copyright © 2020 SipRadius LLC. All right reserved.
 * Author: Gijs Peskens <gijs@in2ip.nl>
 * Author: Sergio Ammirata, Ph.D. <sergio@ammirata.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "psk.h"
#include "log-private.h"
#include "crypto-private.h"
#include <string.h>

#if HAVE_MBEDTLS
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#elif HAVE_NETTLE
#include <nettle/pbkdf2.h>
#include <nettle/aes.h>
#include <nettle/ctr.h>
#elif defined(LINUX_CRYPTO)
#include "linux-crypto.h"
#endif
#if !HAVE_MBEDTLS
#include "fastpbkdf2.h"
#endif

#ifndef AES_BLOCK_SIZE
#define AES_BLOCK_SIZE 16
#endif

#include <stdint.h>

//TODO: handle failures?
int _librist_crypto_psk_rist_key_init(struct rist_key *key, uint32_t key_size, uint32_t rotation, const char *password)
{
	strcpy(key->password, password);
	key->key_size = key_size;
	key->key_rotation = rotation;
#if HAVE_MBEDTLS
	mbedtls_aes_init(&key->mbedtls_aes_ctx);
#elif HAVE_NETTLE
	memset(&key->nettle_ctx, 0, sizeof(key->nettle_ctx));
#elif defined(LINUX_CRYPTO)
	linux_crypto_init(&key->linux_crypto_ctx);
#endif
	return 0;
}

int _librist_crypto_psk_rist_key_destroy(struct rist_key *key)
{
    if (key->key_size) {
#if HAVE_MBEDTLS
	    mbedtls_aes_free(&key->mbedtls_aes_ctx);
#elif HAVE_NETTLE
	//nothing to do here
#elif defined(LINUX_CRYPTO)
	    linux_crypto_free(&key->linux_crypto_ctx);
#endif
    }
	return 0;
}

int _librist_crypto_psk_rist_key_clone(struct rist_key *key_in, struct rist_key *key_out)
{
    strcpy(key_out->password, key_in->password);
    key_out->key_size = key_in->key_size;
    key_out->key_rotation = key_in->key_rotation;
#if HAVE_MBEDTLS
	mbedtls_aes_init(&key_out->mbedtls_aes_ctx);
#elif HAVE_NETTLE
    memset(&key_out->nettle_ctx, 0, sizeof(key_out->nettle_ctx));
#elif defined(LINUX_CRYPTO)
	linux_crypto_init(&key_out->linux_crypto_ctx);
#endif
	return 0;
}

static void _librist_crypto_aes_key(struct rist_key *key)
{
    uint8_t aes_key[256 / 8];
#if HAVE_MBEDTLS
    mbedtls_md_context_t sha_ctx;
    const mbedtls_md_info_t *info_sha;
    int ret = -1;
    /* Setup the hash/HMAC function, for the PBKDF2 function. */
    mbedtls_md_init(&sha_ctx);
    info_sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info_sha == NULL) {
            // rist_log_priv(cctx, RIST_LOG_ERROR, "Failed to setup Mbed TLS
            // hash info\n");
    }

    ret = mbedtls_md_setup(&sha_ctx, info_sha, 1);
    if (ret != 0) {
            // rist_log_priv(cctx, RIST_LOG_ERROR, "Failed to setup Mbed TLS MD
            // ctx");
    }

    ret = mbedtls_pkcs5_pbkdf2_hmac(
        &sha_ctx, (const unsigned char *)key->password, strlen(key->password),
        (const uint8_t *)&key->gre_nonce, sizeof(key->gre_nonce),
        RIST_PBKDF2_HMAC_SHA256_ITERATIONS, key->key_size / 8, aes_key);
    if (ret != 0) {
            // rist_log_priv(cctx, RIST_LOG_ERROR, "Mbed TLS pbkdf2 function
            // failed\n");
    }
    mbedtls_md_free(&sha_ctx);
#elif HAVE_NETTLE
    nettle_pbkdf2_hmac_sha256(strlen(key->password),(const uint8_t*)key->password,
							  RIST_PBKDF2_HMAC_SHA256_ITERATIONS,
							  sizeof(key->gre_nonce), (const uint8_t*)&key->gre_nonce,
							  key->key_size/8, aes_key);
#else
    fastpbkdf2_hmac_sha256(
            (const void *) key->password, strlen(key->password),
            (const void *) &key->gre_nonce, sizeof(key->gre_nonce),
            RIST_PBKDF2_HMAC_SHA256_ITERATIONS,
            aes_key, key->key_size / 8);
#endif


#if HAVE_MBEDTLS
    mbedtls_aes_setkey_enc(&key->mbedtls_aes_ctx, aes_key, key->key_size);
#elif HAVE_NETTLE
	switch(key->key_size) {
	case 256:
        nettle_aes256_set_encrypt_key(&key->nettle_ctx.u.ctx256, aes_key);
        break;
	case 192:
        nettle_aes192_set_encrypt_key(&key->nettle_ctx.u.ctx192, aes_key);
        break;
	case 128:
		RIST_FALLTHROUGH;
	default:
		nettle_aes128_set_encrypt_key(&key->nettle_ctx.u.ctx128, aes_key);
    }
#elif defined(LINUX_CRYPTO)
        if (key->linux_crypto_ctx) linux_crypto_set_key(
            aes_key, key->key_size / 8, key->linux_crypto_ctx);
    else
        aes_key_setup(aes_key, key->aes_key_sched, key->key_size);
#else
    aes_key_setup(aes_key, key->aes_key_sched, key->key_size);
#endif
    key->used_times = 0;
}

static void _librist_crypto_psk_aes_ctr(struct rist_key *key, uint32_t seq_nbe, uint8_t gre_version,const uint8_t inbuf[], uint8_t outbuf[], size_t payload_len)
{
    /* Prepare AES iv */
    uint8_t iv[AES_BLOCK_SIZE];
    // The byte array needs to be zeroes and then the seq in network byte order
	uint8_t copy_offset = gre_version == 1? 0 : 12;
    memset(iv, 0, 16);
    memcpy(iv + copy_offset, &seq_nbe, sizeof(seq_nbe));
#if HAVE_MBEDTLS
        size_t aes_offset = 0;
        unsigned char buf[16];
        mbedtls_aes_crypt_ctr(&key->mbedtls_aes_ctx, payload_len, &aes_offset, iv, buf, inbuf, outbuf);
#elif HAVE_NETTLE
	nettle_cipher_func *f;
	switch(key->key_size) {
	case 256:
		f = (nettle_cipher_func *)nettle_aes256_encrypt;
		break;
	case 192:
		f = (nettle_cipher_func *)nettle_aes192_encrypt;
		break;
	case 128:
		RIST_FALLTHROUGH;
	default:
		f = (nettle_cipher_func *)nettle_aes128_encrypt;
        }
	nettle_ctr_crypt(&key->nettle_ctx.u, f, AES_BLOCK_SIZE, iv,payload_len, outbuf, inbuf);
#elif defined(LINUX_CRYPTO)
        if (key->linux_crypto_ctx)
            linux_crypto_decrypt(inbuf, outbuf, payload_len, iv, key->linux_crypto_ctx);
        else
            aes_decrypt_ctr(inbuf, payload_len, outbuf,
                    key->aes_key_sched, key->key_size, iv);
#else
        aes_decrypt_ctr(inbuf, payload_len, outbuf,
                key->aes_key_sched, key->key_size, iv);
#endif
    key->used_times++;
}

void _librist_crypto_psk_decrypt(struct rist_key *key, uint32_t nonce, uint32_t seq_nbe, uint8_t gre_version,const uint8_t inbuf[], uint8_t outbuf[], size_t payload_len)
{
    if (!nonce)
        return;

    if (nonce != key->gre_nonce) {
        key->gre_nonce = nonce;
        _librist_crypto_aes_key(key);
        key->bad_decryption = false;
        key->bad_count = 0;
    }
    if (key->used_times > RIST_AES_KEY_REUSE_TIMES)
        return;

    _librist_crypto_psk_aes_ctr(key, seq_nbe, gre_version, inbuf, outbuf, payload_len);
    return;
}

void _librist_crypto_psk_encrypt(struct rist_key *key, uint32_t seq_nbe, uint8_t gre_version,const uint8_t inbuf[], uint8_t outbuf[], size_t payload_len)
{
    if (!key->gre_nonce || (key->used_times +1) > RIST_AES_KEY_REUSE_TIMES || (key->key_rotation > 0 && key->used_times >= key->key_rotation)) {
        do {
            key->gre_nonce = prand_u32();
        } while (!key->gre_nonce);
        _librist_crypto_aes_key(key);
    }

    _librist_crypto_psk_aes_ctr(key, seq_nbe, gre_version, inbuf, outbuf, payload_len);
    return;
}

int _librist_crypto_psk_set_passphrase(struct rist_key *key, char *passsphrase, size_t passphrase_len) {
	if (passphrase_len > sizeof(key->password) -1) {
		return -1;
	}
	memcpy(key->password, passsphrase, passphrase_len);
	do {
		key->gre_nonce = prand_u32();
	} while (!key->gre_nonce);
	_librist_crypto_aes_key(key);
	return 0;
}
