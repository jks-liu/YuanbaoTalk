#!/usr/bin/env python3
"""Synchronize release versions using Python 3.11+, without third-party packages.

Usage: python tools/version-bump.py [patch|minor|major|X.Y.Z|current] [--build|--no-build]
Run from any directory. --build builds firmware only; the server runs from source.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
SERVER_VERSION = re.compile(rb'(?m)^__version__ = "([^"\r\n]+)"\r?$')


def validate(version):
    if not VERSION.fullmatch(version) or len(version) > 31:
        raise ValueError("Expected X.Y.Z without leading zeros (maximum 31 ASCII characters).")
    return version


def read_versions(root):
    firmware = root / "firmware/version.txt"
    server = root / "server/yuanbao/__init__.py"
    originals = {firmware: firmware.read_bytes(), server: server.read_bytes()}
    current = validate(originals[firmware].decode("ascii").strip())
    matches = list(SERVER_VERSION.finditer(originals[server]))
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one __version__ assignment in {server}")
    match = matches[0]
    server_version = validate(match[1].decode("ascii"))
    if current != server_version:
        raise ValueError(
            f"Versions differ: firmware={current}, server={server_version}. "
            "Reconcile the two files before retrying."
        )
    return current, originals, match


def build_command(root):
    if os.name == "nt":
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            raise ValueError("Firmware build requires PowerShell.")
        return [shell, "-NoProfile", "-File", str(root / "tools/build-firmware.ps1"), "build"]
    idf = shutil.which("idf.py")
    if not idf:
        raise ValueError("Activate the ESP-IDF environment before using --build.")
    return [sys.executable, idf, "build"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", nargs="?", default="patch")
    build = parser.add_mutually_exclusive_group()
    build.add_argument("--build", action="store_true", help="Build firmware after updating")
    build.add_argument("--no-build", action="store_false", dest="build", help="Default")
    parser.set_defaults(build=False)
    args = parser.parse_args(argv)
    if args.release == "current" and args.build:
        parser.error("current is read-only and cannot be combined with --build")

    try:
        current, originals, match = read_versions(ROOT)
        if args.release == "current":
            print(current)
            return 0
        if args.release in ("major", "minor", "patch"):
            parts = list(map(int, current.split(".")))
            index = ("major", "minor", "patch").index(args.release)
            parts[index] += 1
            parts[index + 1 :] = [0] * (2 - index)
            target = validate(".".join(map(str, parts)))
        else:
            target = validate(args.release)
        command = build_command(ROOT) if args.build else None
        firmware, server = originals
        encoded = target.encode("ascii")
        replacements = {
            firmware: originals[firmware].replace(current.encode("ascii"), encoded, 1),
            server: originals[server][: match.start(1)]
            + encoded
            + originals[server][match.end(1) :],
        }
        # Validate everything before writing, and restore originals on a write error.
        attempted = []
        try:
            for path, content in replacements.items():
                if content != originals[path]:
                    attempted.append(path)
                    path.write_bytes(content)
        except OSError:
            for path in reversed(attempted):
                try:
                    path.write_bytes(originals[path])
                except OSError as error:
                    print(f"Could not restore {path}: {error}", file=sys.stderr)
            raise
        print(f"{current} -> {target}", flush=True)
        if command:
            result = subprocess.run(command, cwd=ROOT / "firmware", check=False)
            if result.returncode:
                print(
                    f"Build failed ({result.returncode}); version remains {target}. "
                    "Retry the build directly, or pass the same exact version with --build.",
                    file=sys.stderr,
                )
                return 1
        return 0
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
