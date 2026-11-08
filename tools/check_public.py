"""Inspect tracked/staged source for accidental private data; never print matched values."""
import argparse
from pathlib import Path
import re
import subprocess
import sys


RULES = {
    "private-key": re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----") ,
    "github-token": re.compile(rb"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})") ,
    "cloudflare-global-key": re.compile(rb"v1\.0-[a-f0-9]{40,}") ,
    "credential-url": re.compile(rb"https?://[^\s/@:]+:[^\s/@]+@") ,
    "windows-profile": re.compile(rb"[A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/](?!Public\b)[^\s\\/]+" , re.I) ,
    "personal-email": re.compile(rb"[A-Za-z0-9_.+-]+@(?:qq\.com|163\.com|126\.com|gmail\.com|outlook\.com|hotmail\.com)" , re.I) ,
}
BLOCKED_PARTS = {".venv" , "__pycache__" , "node_modules" , "wheelhouse" , "data" , "backups" ,
                 "checkpoints" , "exports" , "deploy-state" , "migration"}
BLOCKED_SUFFIXES = {".db" , ".sqlite" , ".sqlite3" , ".csv" , ".log" , ".pem" , ".pfx" , ".p12" ,
                    ".bin" , ".elf" , ".exe" , ".zip" , ".pyc"}


def inspect(entries):
    failures = []
    count = 0
    for name , data in entries:
        count += 1
        path = Path(name)
        if (set(path.parts) & BLOCKED_PARTS or path.suffix.lower() in BLOCKED_SUFFIXES or
                path.name in {"network-config.json" , "project_config.h" , ".env"} or path.name.startswith(".env.")):
            failures.append((name , "private-or-generated-path"))
        for rule , pattern in RULES.items():
            # Upstream author addresses are retained with their original license/source files.
            if rule == "personal-email" and path.parts[0] == "lib":
                continue
            if pattern.search(data):
                failures.append((name , rule))
    for name , rule in failures:
        print("FAIL " + name + " [" + rule + "]")
    print(f"Public-source check: {count} files, {len(failures)} findings")
    return not failures


def sol():
    parser = argparse.ArgumentParser()
    parser.add_argument("--staged" , action = "store_true" , help = "Read Git index blobs instead of working files")
    parser.add_argument("--directory" , type = Path , help = "Inspect an extracted source archive without Git")
    args = parser.parse_args()
    root = args.directory.resolve() if args.directory else Path(__file__).resolve().parents[1]
    if args.directory:
        entries = ((p.relative_to(root).as_posix() , p.read_bytes()) for p in sorted(root.rglob("*")) if p.is_file())
    else:
        names = subprocess.check_output(["git" , "ls-files" , "-z"] , cwd = root).decode("utf-8").split("\0")
        entries = ((name , subprocess.check_output(["git" , "show" , ":" + name] , cwd = root)
                    if args.staged else (root / name).read_bytes()) for name in names if name)
    sys.exit(0 if inspect(entries) else 1)


if __name__ == "__main__":
    sol()
