"""Package only tracked files after the public-source check. Output outside the repository."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
import zipfile

from check_public import inspect


def sol():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("output" , type = Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if root == output or root in output.parents:
        raise ValueError("Choose an output path outside the source repository")
    if subprocess.run(["git" , "diff" , "--quiet" , "HEAD" , "--"] , cwd = root).returncode != 0:
        raise ValueError("Commit tracked changes before packaging a release")
    output.parent.mkdir(parents = True , exist_ok = True)
    subprocess.run(["git" , "archive" , "--format=zip" , "--prefix=nfc-attendance-v1.0/" ,
                    "--output=" + str(output) , "HEAD"] , cwd = root , check = True)
    with tempfile.TemporaryDirectory(prefix = "nfc-source-check-") as directory:
        with zipfile.ZipFile(output) as archive:
            entries = [(item.filename.removeprefix("nfc-attendance-v1.0/") , archive.read(item))
                       for item in archive.infolist() if not item.is_dir()]
            archive.extractall(directory)
        unpacked = Path(directory) / "nfc-attendance-v1.0"
        if not inspect((p.relative_to(unpacked).as_posix() , p.read_bytes()) for p in unpacked.rglob("*") if p.is_file()):
            output.unlink()
            raise SystemExit(1)
    print(f"Files: {len(entries)}, source bytes: {sum(len(data) for name , data in entries)}, zip bytes: {output.stat().st_size}")
    print("SHA256: " + hashlib.sha256(output.read_bytes()).hexdigest())


if __name__ == "__main__":
    sol()
