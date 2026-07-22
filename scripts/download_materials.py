#!/usr/bin/env python3
"""Download the PBR materials used by the Lumin Engine sandbox."""

from __future__ import annotations

import argparse
import hashlib
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class MaterialFile:
    relative_path: str
    url: str
    sha256: str


MATERIAL_FILES = (
    MaterialFile(
        "aerial_asphalt_01/aerial_asphalt_01_diff_1k.jpg",
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/aerial_asphalt_01/"
        "aerial_asphalt_01_diff_1k.jpg",
        "28bfd71288b364f825e0feb750e487410422d80bf3c31d32da01cae3bf085b58",
    ),
    MaterialFile(
        "aerial_asphalt_01/aerial_asphalt_01_nor_gl_1k.png",
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/aerial_asphalt_01/"
        "aerial_asphalt_01_nor_gl_1k.png",
        "7fe4527733954080a5affa1a690f04b21532e092ddd5b6d89e96c9dd07ae61fe",
    ),
    MaterialFile(
        "aerial_asphalt_01/aerial_asphalt_01_rough_1k.jpg",
        "https://dl.polyhaven.org/file/ph-assets/Textures/jpg/1k/aerial_asphalt_01/"
        "aerial_asphalt_01_rough_1k.jpg",
        "19ca745417e36a1cd1114411801933b3144a351d1409d0d9caa1046b9a20c0ec",
    ),
)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_file(material_file: MaterialFile, destination: Path, force: bool) -> None:
    if destination.is_file() and not force and file_sha256(destination) == material_file.sha256:
        print(f"Up to date: {destination}")
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = destination.with_name(destination.name + ".part")
    request = urllib.request.Request(material_file.url, headers={"User-Agent": "LuminEngine-material-downloader/1.0"})

    print(f"Downloading: {material_file.url}")
    try:
        with urllib.request.urlopen(request, timeout=60) as response, temporary_path.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)

        actual_hash = file_sha256(temporary_path)
        if actual_hash != material_file.sha256:
            raise RuntimeError(
                f"SHA-256 mismatch for {destination.name}: expected {material_file.sha256}, got {actual_hash}"
            )
        temporary_path.replace(destination)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise


def check_materials(material_root: Path) -> bool:
    valid = True
    for material_file in MATERIAL_FILES:
        destination = material_root / material_file.relative_path
        if not destination.is_file():
            print(f"Missing: {destination}", file=sys.stderr)
            valid = False
        elif file_sha256(destination) != material_file.sha256:
            print(f"Invalid checksum: {destination}", file=sys.stderr)
            valid = False
        else:
            print(f"OK: {destination}")
    return valid


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="download files even when the local checksum is correct")
    parser.add_argument("--check", action="store_true", help="verify local files without accessing the network")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    repository_root = Path(__file__).resolve().parent.parent
    material_root = repository_root / "assets" / "materials"

    if arguments.check:
        return 0 if check_materials(material_root) else 1

    try:
        for material_file in MATERIAL_FILES:
            download_file(material_file, material_root / material_file.relative_path, arguments.force)
    except (OSError, RuntimeError) as error:
        print(f"Material download failed: {error}", file=sys.stderr)
        return 1

    print("All materials are ready.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
