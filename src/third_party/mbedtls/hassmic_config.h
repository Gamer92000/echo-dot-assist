/*
 * mbedTLS 3.6 configuration for hassmic (MBEDTLS_CONFIG_FILE): a TLS 1.2 and 1.3 client and nothing else.
 *
 * Why it is here at all (issue #236): the firmwares' own libcurl runs on Android 7's BoringSSL (donut, biscuit, radar,
 * cannoli, checkers, cronos, crown, cupcake: no TLS 1.3 code in libssl.so) or Android 9's (athena: only the draft
 * variants, SSL_set_tls13_variant), so a Home Assistant on the "Modern" SSL profile, TLS 1.3 alone, refused every
 * reply's audio.  tls.c is hassmic's one TLS client now: media, online updates, the plugin index, Amazon's downloads,
 * Music Assistant over wss://.
 *
 * What a server may pick: TLS 1.3 with AES-GCM or ChaCha20-Poly1305 and X25519/P-256/P-384 key shares; TLS 1.2 with
 * ECDHE and AES-GCM or ChaCha20-Poly1305 (what Mozilla's "intermediate" profile offers; no CBC, no static RSA key
 * exchange: neither GitHub, Amazon nor Home Assistant's profiles need them).  Certificates: RSA (PKCS#1 v1.5 and PSS)
 * and ECDSA, SHA-256/384/512 (SHA-1 only as the hash some old roots are signed with; a trust anchor's own signature is
 * never checked).  The time is checked (validity), as BoringSSL did.
 *
 * Randomness: the kernel's (/dev/urandom; tls.c), for the TLS layer and for PSA (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG): no
 * entropy pool or DRBG of mbedTLS's own to seed and lock.  PSA's key store is shared by every connection's thread:
 * MBEDTLS_THREADING_PTHREAD.
 */
#ifndef HASSMIC_MBEDTLS_CONFIG_H
#define HASSMIC_MBEDTLS_CONFIG_H

/* platform */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_FS_IO
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_ERROR_C

/* PSA, which TLS 1.3 runs on */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_USE_PSA_CRYPTO
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC

/* symmetric */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_HKDF_C

/* public key */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* certificates */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
/* RSA-PSS: without it no rsa_pss_rsae_* goes into the ClientHello, which a TLS 1.3 server with an RSA certificate must
 * sign with (RFC 8446 4.2.3): GitHub's release-assets CDN answered such a hello with a record nobody could read */
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* TLS, client side */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES
#define MBEDTLS_SSL_SESSION_TICKETS         /* a TLS 1.3 server's NewSessionTicket is read (and not kept) */
/* A record from the server may be the protocol's whole 16 kB (no server here negotiates smaller ones); ours are
 * requests and WebSocket frames, cut into 4 kB records */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#endif
