"""Run S3 firmware self-tests in QEMU (no physical hardware).

Usage: python tools/run-selftest.py [--build firmware/build-selftest] [--qemu PATH]
Run build-selftest.ps1 first. The test never flashes or erases a connected device.
"""

import argparse
import json
from pathlib import Path
import queue
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("firmware/build-selftest"))
    parser.add_argument("--trace", action="store_true", help="Save QEMU guest errors for diagnosis")
    parser.add_argument(
        "--qemu",
        default="C:/Espressif/tools/qemu-xtensa/esp_develop_9.2.2_20260417/qemu/bin/qemu-system-xtensa.exe",
    )
    args = parser.parse_args()
    build = args.build.resolve()
    settings = json.loads((build / "config/sdkconfig.json").read_text())
    if not settings.get("YT_SELF_TEST"):
        raise SystemExit("Refusing non-self-test image: build with YT_SELF_TEST enabled")
    layout = json.loads((build / "flasher_args.json").read_text())
    flash = bytearray(b"\xff") * (8 * 1024 * 1024)
    for address, filename in layout["flash_files"].items():
        data = (build / filename).read_bytes()
        start = int(address, 16)
        if start + len(data) > len(flash):
            raise SystemExit("Flash layout exceeds 8 MB test image")
        flash[start : start + len(data)] = data
    image = build / "qemu-flash.bin"
    image.write_bytes(flash)
    cmd = [
        args.qemu,
        "-M",
        "esp32s3",
        "-m",
        "16M",
        "-drive",
        f"file={image},if=mtd,format=raw",
        "-global",
        "driver=ssi_psram,property=is_octal,value=true",
        "-global",
        "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
        "-display",
        "none",
        "-serial",
        "stdio",
        "-monitor",
        "none",
        "-nic",
        "none",
        "-no-reboot",
    ]
    if args.trace:
        cmd += ["-d", "guest_errors,unimp", "-D", str(build / "qemu-debug.log")]
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    lines = queue.Queue()

    def read():
        for line in proc.stdout:
            lines.put(line)
        lines.put(None)

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    log = []
    passed = False
    try:
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                line = lines.get(timeout=min(1, max(0.01, deadline - time.monotonic())))
            except queue.Empty:
                continue
            if line is None:
                break
            log.append(line)
            if "YUANBAO SELFTEST PASS" in line:
                passed = True
                break
            if "assert failed" in line or "Guru Meditation" in line:
                deadline = min(deadline, time.monotonic() + 3)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        reader.join(timeout=2)
        (build / "selftest.log").write_text("".join(log), encoding="utf-8")
    print("".join(log[-45:]))
    if not passed:
        print(f"Self-test failed or timed out (QEMU exit code {proc.returncode})")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
