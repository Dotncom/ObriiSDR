"""Independent public-test-key fixtures, using PyCryptodome (not Obrii crypto)."""
from pathlib import Path
from hashlib import sha256
import hmac
import struct
from Cryptodome.Cipher import AES
from Cryptodome.Protocol.DH import import_x25519_private_key, key_agreement

root = Path(__file__).resolve().parents[1]
alice = bytes.fromhex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a')
bob = bytes.fromhex('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb')
a = import_x25519_private_key(alice)
b = import_x25519_private_key(bob)
shared = key_agreement(static_priv=a, static_pub=b.public_key(), kdf=lambda x: x)
key = sha256(shared).digest()
sender, recipient, packet_id = 0x11223344, 0x55667788, 0x12345678
extra = bytes.fromhex('f0e1d2c3')
nonce = struct.pack('<I', packet_id) + extra + struct.pack('<I', sender) + b'\0'
plain = bytes.fromhex('0801120568656c6c6f')
ccm = AES.new(key, AES.MODE_CCM, nonce=nonce, mac_len=8)
cipher, tag = ccm.encrypt_and_digest(plain)
header = struct.pack('<III4B', recipient, sender, packet_id, 0x6b, 0, 0, 0x44)
wire = header + cipher + tag + extra
proof = hmac.new(key, b'ack' + struct.pack('<III', recipient, sender, packet_id) + bytes.fromhex('1800'), 'sha256').digest()[:8]
values = dict(alice_private=alice, alice_public=a.public_key().export_key(format='raw'),
              bob_private=bob, bob_public=b.public_key().export_key(format='raw'), shared=shared,
              aes_key=key, nonce=nonce, plaintext=plain, cipher_tag=cipher+tag, radio=wire, ack_proof=proof)
path = root / 'src/meshtastic/pki_vectors.tsv'
path.write_text('# Public RFC7748 keys; independently generated with PyCryptodome\n' +
                ''.join(f'{name}|{value.hex()}\n' for name,value in values.items()), encoding='utf-8')
print(f'Independent PKI vectors: {path}')
