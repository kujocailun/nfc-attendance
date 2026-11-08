from pathlib import Path
import struct

import certifi
from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding , PublicFormat


def sol():
    root = Path(__file__).resolve().parents[1]
    certs = x509.load_pem_x509_certificates(Path(certifi.where()).read_bytes())
    pairs = sorted({(c.subject.public_bytes() , c.public_key().public_bytes(Encoding.DER , PublicFormat.SubjectPublicKeyInfo)) for c in certs})
    if len({subject for subject , key in pairs}) != len(pairs):
        raise ValueError("Duplicate root subjects require review")
    packet = struct.pack(">H" , len(pairs))
    for subject , key in pairs:
        packet += struct.pack(">HH" , len(subject) , len(key)) + subject + key
    target = root / "certs" / "x509_crt_bundle"
    target.parent.mkdir(exist_ok = True)
    target.write_bytes(packet)
    print(f"{len(pairs)} trusted roots, {len(packet)} bytes; source certifi {certifi.__version__}")


if __name__ == "__main__":
    sol()
