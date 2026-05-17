"""NCM (Netease Cloud Music) file decryptor — correct implementation."""
import os
import json
import struct
import base64
import binascii

from Crypto.Cipher import AES

# Core key for RC4 key decryption
_CORE_KEY = binascii.a2b_hex("687A4852416D736F356B496E62617857")
# Meta key for metadata decryption
_META_KEY = binascii.a2b_hex("2331346C6A6B5F215C5D2630553C2728")

_MAGIC = b"CTENFDAM"


def _unpad(s: bytes) -> bytes:
    pad = s[-1] if isinstance(s[-1], int) else ord(s[-1])
    return s[:-pad]


def decrypt_ncm(input_path: str, output_dir: str) -> tuple:
    """Decrypt an NCM file.
    
    Returns (output_path, audio_format) e.g. ('.../song.mp3', 'mp3').
    Raises RuntimeError on failure.
    """
    with open(input_path, "rb") as f:
        data = f.read()

    if data[:8] != _MAGIC:
        raise RuntimeError("Not a valid NCM file (bad magic)")

    pos = 10  # skip 8-byte magic + 2-byte gap

    # --- RC4 key ---------------------------------------------------
    key_len = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    key_data = bytearray(data[pos:pos + key_len])
    pos += key_len

    for i in range(len(key_data)):
        key_data[i] ^= 0x64
    cryptor = AES.new(_CORE_KEY, AES.MODE_ECB)
    key_data = _unpad(cryptor.decrypt(bytes(key_data)))[17:]

    key_box = bytearray(range(256))
    c = 0
    last_byte = 0
    key_offset = 0
    key_len = len(key_data)
    for i in range(256):
        swap = key_box[i]
        c = (swap + last_byte + key_data[key_offset]) & 0xFF
        key_offset += 1
        if key_offset >= key_len:
            key_offset = 0
        key_box[i] = key_box[c]
        key_box[c] = swap
        last_byte = c

    # --- Meta data -------------------------------------------------
    meta_len = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    meta_data = bytearray(data[pos:pos + meta_len])
    pos += meta_len

    for i in range(len(meta_data)):
        meta_data[i] ^= 0x63
    meta_raw = base64.b64decode(bytes(meta_data)[22:])
    cryptor = AES.new(_META_KEY, AES.MODE_ECB)
    meta_json = _unpad(cryptor.decrypt(meta_raw)).decode("utf-8")[6:]
    meta = json.loads(meta_json)
    audio_format = meta.get("format", "mp3")

    # Skip CRC (4) + gap (5) + album cover
    pos += 4 + 5
    image_size = struct.unpack_from("<I", data, pos)[0]
    pos += 4 + image_size

    # --- Decrypt audio ---------------------------------------------
    audio_data = data[pos:]
    decrypted = bytearray(len(audio_data))
    for i in range(1, len(audio_data) + 1):
        j = i & 0xFF
        decrypted[i - 1] = audio_data[i - 1] ^ key_box[
            (key_box[j] + key_box[(key_box[j] + j) & 0xFF]) & 0xFF
        ]

    base_name = os.path.splitext(os.path.basename(input_path))[0]
    output_path = os.path.join(output_dir, f"{base_name}.{audio_format}")
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    with open(output_path, "wb") as f:
        f.write(decrypted)

    if os.path.getsize(output_path) == 0:
        os.remove(output_path)
        raise RuntimeError("Decrypted output is empty")

    return output_path, audio_format


def detect_format(filepath: str) -> str:
    """Detect audio format from file header. Returns 'flac', 'mp3', or 'unknown'."""
    with open(filepath, "rb") as f:
        header = f.read(4)

    if header[:4] == b"fLaC":
        return "flac"
    if header[:3] == b"ID3":
        return "mp3"
    if len(header) >= 1 and header[0] == 0xFF and (header[1] & 0xE0) == 0xE0:
        return "mp3"
    return "unknown"
