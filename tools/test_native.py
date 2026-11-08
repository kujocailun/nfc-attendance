"""Compile and run the firmware's native regression programs without touching a device."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def sol():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler" , default = os.environ.get("CXX" , "g++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    targets = ("attendance" , "network" , "button" , "feedback" , "storage")
    with tempfile.TemporaryDirectory(prefix = "nfc-native-") as directory:
        for name in targets:
            folder = root / "test" / (name + "_host")
            output = Path(directory) / (name + (".exe" if os.name == "nt" else ""))
            include = root / "test" / "attendance_host" if name == "network" else folder
            subprocess.run([args.compiler , "-std=c++17" , "-Wall" , "-Wextra" , "-Werror" ,
                            "-I" , str(include) , str(folder / "main.cpp") , "-o" , str(output)] , check = True)
            subprocess.run([str(output)] , check = True)
            print("PASS " + name , flush = True)


if __name__ == "__main__":
    sol()
