"""Builds and installs ZMenu WaW.

    python build.py            build the DLL and copy it, the script and the ini into the game folder
    python build.py --remove   uninstall

The game folder (the one holding CoDWaW.exe) comes from the WAW_DIR environment variable.

Needs the Zig compiler:  pip install ziglang
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("WAW_DIR", "")
INSTALLED = ("d3d9.dll", "ZMenuWaW.gsc", "ZMenuWaW.ini")

if not os.path.isfile(os.path.join(GAME, "CoDWaW.exe")):
    sys.exit("Set WAW_DIR to your game folder (the one containing CoDWaW.exe).")


def build():
    out = os.path.join(HERE, "build")
    os.makedirs(out, exist_ok=True)
    # not named d3d9.dll at link time: zig mistakes that for its own d3d9 import library
    dll = os.path.join(out, "zmenu.dll")
    subprocess.check_call([
        sys.executable, "-m", "ziglang", "c++", "-target", "x86-windows-gnu", "-shared", "-O2", "-s",
        "-fno-exceptions", "-fno-rtti", "-w", "-o", dll, os.path.join(HERE, "src", "zmenu.cpp"),
        "-lgdi32", "-luser32", "-lkernel32"])

    shutil.copy(dll, os.path.join(GAME, "d3d9.dll"))
    shutil.copy(os.path.join(HERE, "ZMenuWaW.gsc"), GAME)
    if not os.path.exists(os.path.join(GAME, "ZMenuWaW.ini")):
        shutil.copy(os.path.join(HERE, "ZMenuWaW.ini"), GAME)
    print("installed ->", GAME)


def remove():
    for name in INSTALLED:
        path = os.path.join(GAME, name)
        if os.path.exists(path):
            os.remove(path)
    print("removed")


if __name__ == "__main__":
    if "--remove" in sys.argv:
        remove()
    else:
        build()
