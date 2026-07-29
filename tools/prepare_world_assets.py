#!/usr/bin/env python3
"""Prepare MMVP's modified vanilla world-screen assets."""

from __future__ import annotations

import argparse
from pathlib import Path


TV_SOURCE_TEXTURES = (
    rb"textures\Effects\TVAnim\PleaseStandByFull01_d.dds",
    rb"Textures\Effects\TVAnim\PleaseStandByFull01_d.dds",
)
TV_TARGET_TEXTURE = rb"textures\MMVP\World\TelevisionScreenOutput1_d.dds"
SCREEN_SOURCE_MATERIAL = (
    rb"C:\projects\Fallout4\Build\PC\Data\materials"
    rb"\SetDressing\Signage\BillboardSignDefault01.BGSM"
)
SCREEN_TARGET_MATERIAL = (
    rb"Materials\MMVP\World"
    rb"\MovieScreenProjectionSurface_BillboardSignDefault01ReplacementMMVP.BGSM"
)
SCREEN_SOURCE_TEXTURE = rb"setdressing/signage/billboardsigndefault01_d.dds"
SCREEN_TARGET_TEXTURE = rb"MMVP/World/MovieProjectorScreenOutput00001_d.dds"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Patch Fallout 4's vanilla TV and drive-in screen meshes for MMVP. "
            "The inputs should be folders unpacked with Rust-BSA-BA2-Handler."
        )
    )
    parser.add_argument("--mesh-root", required=True, type=Path)
    parser.add_argument("--material-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    return parser.parse_args()


def find_case_insensitive(root: Path, relative: str) -> Path:
    current = root
    for part in Path(relative).parts:
        matches = [
            child
            for child in current.iterdir()
            if child.name.casefold() == part.casefold()
        ]
        if len(matches) != 1:
            raise FileNotFoundError(
                f"Could not uniquely locate '{relative}' below '{root}'."
            )
        current = matches[0]
    if not current.is_file():
        raise FileNotFoundError(current)
    return current


def patch_exact(
    source: Path,
    destination: Path,
    replacements: tuple[tuple[bytes, bytes], ...],
) -> None:
    data = source.read_bytes()
    for old, new in replacements:
        if len(old) != len(new):
            raise ValueError(
                f"Replacement lengths differ ({len(old)} != {len(new)}): "
                f"{old!r} -> {new!r}"
            )
        count = data.count(old)
        if count != 1:
            raise ValueError(
                f"Expected one occurrence of {old!r} in {source}, found {count}."
            )
        data = data.replace(old, new, 1)

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"Prepared {destination}")


def copy_exact(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(source.read_bytes())
    print(f"Prepared {destination}")


def main() -> int:
    args = parse_args()
    mesh_root = args.mesh_root.resolve()
    material_root = args.material_root.resolve()
    output_root = args.output_root.resolve()

    tv_variants = (
        "Meshes/SetDressing/Workshop/TelevisionWorkshopVariant01.nif",
        "Meshes/SetDressing/Workshop/TelevisionWorkshopVariant02.nif",
    )
    for relative in tv_variants:
        source = find_case_insensitive(mesh_root, relative)
        target = output_root / "Meshes/MMVP" / Path(relative).name
        source_texture = next(
            (candidate for candidate in TV_SOURCE_TEXTURES if candidate in source.read_bytes()),
            None,
        )
        if source_texture is None:
            raise ValueError(f"Could not find the vanilla TV screen texture in {source}.")
        patch_exact(source, target, ((source_texture, TV_TARGET_TEXTURE),))

    projector = find_case_insensitive(
        mesh_root,
        "Meshes/Props/FilmProjector/FilmProjector.nif",
    )
    copy_exact(projector, output_root / "Meshes/MMVP/FilmProjector.nif")

    screen = find_case_insensitive(
        mesh_root,
        "Meshes/SetDressing/DriveIn/DriveinScreen01.nif",
    )
    patch_exact(
        screen,
        output_root / "Meshes/MMVP/MovieScreen.nif",
        ((SCREEN_SOURCE_MATERIAL, SCREEN_TARGET_MATERIAL),),
    )

    material = find_case_insensitive(
        material_root,
        "Materials/SetDressing/Signage/BillboardSignDefault01.BGSM",
    )
    patch_exact(
        material,
        output_root
        / "Materials/MMVP/World"
        / "MovieScreenProjectionSurface_BillboardSignDefault01ReplacementMMVP.BGSM",
        ((SCREEN_SOURCE_TEXTURE, SCREEN_TARGET_TEXTURE),),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
