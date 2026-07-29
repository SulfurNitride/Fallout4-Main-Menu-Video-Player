#!/usr/bin/env python3
"""Generate an uncompressed BGRA8 DDS recognized by MMVP's D3D11 hook."""

from __future__ import annotations

import argparse
import pathlib
import struct


MARKERS = {
    "television": bytes(
        [
            ord("M"),
            ord("M"),
            ord("V"),
            ord("P"),
            ord("T"),
            ord("V"),
            ord("0"),
            ord("1"),
            0x17,
            0x42,
            0xA5,
            0xE1,
            0x4D,
            0x4D,
            0x56,
            0x50,
        ]
    ),
    "projector": bytes(
        [
            ord("M"),
            ord("M"),
            ord("V"),
            ord("P"),
            ord("M"),
            ord("O"),
            ord("V"),
            ord("I"),
            0x32,
            0x88,
            0xC4,
            0x7F,
            0x50,
            0x56,
            0x4D,
            0x4D,
        ]
    ),
}


def build_header(width: int, height: int) -> bytes:
    ddsd_caps = 0x1
    ddsd_height = 0x2
    ddsd_width = 0x4
    ddsd_pitch = 0x8
    ddsd_pixel_format = 0x1000
    ddpf_alpha_pixels = 0x1
    ddpf_rgb = 0x40
    dds_caps_texture = 0x1000

    values = [
        124,
        ddsd_caps
        | ddsd_height
        | ddsd_width
        | ddsd_pitch
        | ddsd_pixel_format,
        height,
        width,
        width * 4,
        0,
        0,
        *([0] * 11),
        32,
        ddpf_alpha_pixels | ddpf_rgb,
        0,
        32,
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        0xFF000000,
        dds_caps_texture,
        0,
        0,
        0,
        0,
    ]
    return b"DDS " + struct.pack("<31I", *values)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=MARKERS, required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    if args.width < 4 or args.height < 1:
        raise SystemExit("DDS dimensions must contain the 16-byte marker")

    pixel_bytes = bytearray(args.width * args.height * 4)
    for offset in range(3, len(pixel_bytes), 4):
        pixel_bytes[offset] = 0xFF
    pixel_bytes[:16] = MARKERS[args.kind]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(
        build_header(args.width, args.height) + pixel_bytes
    )


if __name__ == "__main__":
    main()
