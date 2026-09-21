import os, subprocess, sys
from noise.connection import NoiseConnection, Keypair
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from cryptography.hazmat.primitives import serialization as ser
raw = lambda k: k.private_bytes(ser.Encoding.Raw, ser.PrivateFormat.Raw, ser.NoEncryption())
pub = lambda k: k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
si, sr = X25519PrivateKey.generate(), X25519PrivateKey.generate(); psk = os.urandom(32); prologue = b'{"type":"client/init"}{"type":"server/init"}'
n = NoiseConnection.from_name(b"Noise_KKpsk2_25519_ChaChaPoly_SHA256"); n.set_as_initiator(); n.set_psks(psk); n.set_prologue(prologue)
n.set_keypair_from_private_bytes(Keypair.STATIC, raw(si)); n.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, pub(sr)); n.start_handshake()
p = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def tx(b): p.stdin.write(b.hex() + "\n"); p.stdin.flush()
def rx(): l = p.stdout.readline().strip(); assert not l.startswith("ERR"), l; return bytes.fromhex(l)
for v in (prologue, raw(sr), pub(si), psk): tx(v)
tx(n.write_message(b'{"psk_id":"x","psk_category":"sn"}')); assert rx() == b'{"psk_id":"x","psk_category":"sn"}', "msg1 payload"
assert n.read_message(rx()) == b"{}", "msg2 payload"; assert n.handshake_finished
assert rx() == n.get_handshake_hash(), "handshake hash"
for m in (b"hello", b"x" * 3000, b""):
    tx(n.encrypt(m)); assert n.decrypt(rx()) == m[::-1]
print("Noise KKpsk2 responder interoperates with python noiseprotocol (handshake, hash, transport both ways)")
