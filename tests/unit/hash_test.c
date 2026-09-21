#include <stdio.h>
#include <string.h>
#include "hash.h"
static void hex(const uint8_t *p, int n, char *o) { for (int i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", p[i]); }
int main(void) {
    uint8_t d[32]; char h[80], b[128]; int bad = 0;
    sha256("abc", 3, d); hex(d, 32, h); bad |= strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("sendspin-sentinel-psk-v1", 24, d); hex(d, 32, h); bad |= 2 * !!strcmp(h, "1b5e24dbc1aed95fc2a5a338a90c05df44bd10f5ec1f4cd66cbf86272767b9d3");
    { uint8_t in[18 + 32]; memcpy(in, "sendspin-psk-id-v1", 18); memcpy(in + 18, d, 32); sha256(in, 50, d); b64_encode(d, 32, b, 1, 0);
      bad |= 4 * !!strcmp(b, "GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo"); }
    hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want ", 16, "for nothing?", 12, d); hex(d, 32, h);
    bad |= 8 * !!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    sha1("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 60, d); b64_encode(d, 20, b, 0, 1);
    bad |= 16 * !!strcmp(b, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    { uint8_t o[8]; long n = b64_decode("aGVsbG8", 7, o, 8); bad |= 32 * !(n == 5 && !memcmp(o, "hello", 5)); }
    printf(bad ? "FAIL mask %d\n" : "hash/base64 vectors ok (incl. Sendspin sentinel PSK and psk_id from the spec)\n", bad);
    return bad;
}
