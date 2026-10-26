#!/usr/bin/env python3
"""Create a unique setup passphrase and a 64 KiB factory NVS image.

This command only creates local files. It never contacts or flashes a device.
Run with the Python environment supplied by the selected ESP-IDF installation,
or select that interpreter using --idf-python. Secret files are intentionally
written beneath the repository's ignored tools/provisioning-private directory.
"""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys


OUTPUT_ROOT = Path(__file__).resolve().parent / "provisioning-private"


def write_private(path: Path, content: str) -> None:
    """Create, never overwrite, a secret file; request owner-only POSIX access."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output:
        output.write(content)


def provision(device_id: str, idf_path: Path, idf_python: str, output_root: Path = OUTPUT_ROOT) -> Path:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", device_id):
        raise ValueError("Device label must contain 1-64 ASCII letters, digits, underscores, or hyphens, starting with a letter or digit.")
    if device_id.upper().split(".")[0] in {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(10)), *(f"LPT{i}" for i in range(10))}:
        raise ValueError("Device label is a reserved filesystem name.")
    generator = idf_path.resolve() / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
    if not generator.is_file():
        raise ValueError("ESP-IDF NVS partition generator was not found in the selected installation.")
    # Windows inherits the workspace ACL. Its Python 3.12 mode=0700 ACL rewrite
    # can remove an app-container's own access; POSIX receives owner-only mode.
    directory_mode = 0o777 if os.name == "nt" else 0o700
    output_root.mkdir(mode=directory_mode, parents=True, exist_ok=True)
    destination = output_root / device_id
    # mkdir is the ownership claim: a rerun must not replace a deployed password.
    destination.mkdir(mode=directory_mode, exist_ok=False)
    password = secrets.token_urlsafe(24)
    csv_path = destination / "factory.csv"
    descriptor = os.open(csv_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["key", "type", "encoding", "value"])
        writer.writerow(["provision", "namespace", "", ""])
        writer.writerow(["setup_password", "data", "string", password])
    write_private(destination / "setup-password.txt", password + "\n")
    image = destination / "factory.bin"
    try:
        result = subprocess.run(
            [idf_python, str(generator), "generate", str(csv_path), str(image), "0x10000"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError:
        raise RuntimeError("Could not start the selected ESP-IDF Python interpreter; private source files remain in the new directory.") from None
    # Generator diagnostics may contain input values. Never replay them to a log.
    if result.returncode != 0 or not image.is_file() or image.stat().st_size != 0x10000:
        raise RuntimeError("Factory image generation failed. Check the ESP-IDF Python environment; private source files remain in the new directory.")
    os.chmod(image, 0o600)
    write_private(
        destination / "README.txt",
        "Factory partition: 0x1a000; length: 0x10000 (65536 bytes).\n"
        "Image contains the unique setup passphrase in namespace provision, key setup_password.\n"
        "Keep all files in this directory private and outside version control.\n"
        "Read setup-password.txt locally to join the board's setup network.\n"
        "Normal application updates must preserve factory NVS, tracker NVS, and tracker_queue.\n"
        "This tool has not flashed or contacted any hardware.\n",
    )
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-id", required=True, help="Filesystem label for this board's private provisioning bundle")
    parser.add_argument("--idf-path", type=Path, default=Path(os.environ["IDF_PATH"]) if os.environ.get("IDF_PATH") else None)
    parser.add_argument("--idf-python", default=sys.executable, help="ESP-IDF Python interpreter (default: current interpreter)")
    args = parser.parse_args()
    if args.idf_path is None:
        parser.error("Set IDF_PATH or supply --idf-path.")
    try:
        destination = provision(args.device_id, args.idf_path, args.idf_python)
    except FileExistsError:
        parser.exit(1, "Refusing to overwrite an existing provisioning directory. Reuse its saved image and passphrase.\n")
    except (ValueError, RuntimeError) as error:
        parser.exit(1, f"{error}\n")
    except OSError:
        parser.exit(1, "Could not create private provisioning files; inspect permissions and available space.\n")
    print(f"Created private provisioning bundle: {destination}")
    print("Factory image: factory.bin (65536 bytes, partition offset 0x1a000).")
    print("Read setup-password.txt locally; the passphrase is not printed in command output.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
