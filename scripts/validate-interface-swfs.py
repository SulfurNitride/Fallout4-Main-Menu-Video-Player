#!/usr/bin/env python3

import argparse
import math
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class SwfInfo:
    path: Path
    version: int
    width: float
    height: float
    frame_rate: float
    frame_count: int
    body: bytes


class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.position = 0

    def read(self, count: int, signed: bool = False) -> int:
        value = 0
        for _ in range(count):
            byte_index = self.position // 8
            bit_index = 7 - self.position % 8
            value = (value << 1) | ((self.data[byte_index] >> bit_index) & 1)
            self.position += 1
        if signed and count > 0 and value & (1 << (count - 1)):
            value -= 1 << count
        return value


def read_swf(path: Path) -> SwfInfo:
    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError("file is too small to be a SWF")

    signature = data[:3]
    version = data[3]
    declared_length = struct.unpack_from("<I", data, 4)[0]
    compressed = data[8:]
    if signature == b"FWS":
        body = compressed
    elif signature == b"CWS":
        body = zlib.decompress(compressed)
    elif signature == b"ZWS":
        raise ValueError("LZMA-compressed ZWS files are not supported")
    else:
        raise ValueError(f"unsupported SWF signature {signature!r}")

    if declared_length != len(body) + 8:
        raise ValueError(
            f"declared length {declared_length} does not match "
            f"decoded length {len(body) + 8}")

    bits = BitReader(body)
    field_bits = bits.read(5)
    x_min = bits.read(field_bits, signed=True)
    x_max = bits.read(field_bits, signed=True)
    y_min = bits.read(field_bits, signed=True)
    y_max = bits.read(field_bits, signed=True)
    rect_bytes = math.ceil(bits.position / 8)

    if len(body) < rect_bytes + 4:
        raise ValueError("SWF header is truncated after its frame rectangle")

    frame_rate_raw = struct.unpack_from("<H", body, rect_bytes)[0]
    frame_count = struct.unpack_from("<H", body, rect_bytes + 2)[0]
    return SwfInfo(
        path=path,
        version=version,
        width=(x_max - x_min) / 20.0,
        height=(y_max - y_min) / 20.0,
        frame_rate=frame_rate_raw / 256.0,
        frame_count=frame_count,
        body=body,
    )


def require_symbols(info: SwfInfo, symbols: tuple[str, ...]) -> list[str]:
    missing = []
    for symbol in symbols:
        if symbol.encode("utf-8") not in info.body:
            missing.append(symbol)
    return missing


def validate(
    path: Path,
    root_class: str,
    extra_symbols: tuple[str, ...],
) -> list[str]:
    errors = []
    try:
        info = read_swf(path)
    except (OSError, ValueError, zlib.error) as error:
        return [f"{path}: {error}"]

    if info.version != 10:
        errors.append(f"SWF version is {info.version}, expected 10")
    if abs(info.width - 826.0) > 0.05 or abs(info.height - 700.0) > 0.05:
        errors.append(
            f"Stage is {info.width:g}x{info.height:g}, expected 826x700")
    if abs(info.frame_rate - 60.0) > 0.01:
        errors.append(
            f"frame rate is {info.frame_rate:g}, expected 60")
    if b"\x10\x00\x2e\x00" not in info.body:
        errors.append("ABC 46 signature was not found")

    common_symbols = (
        root_class,
        "BGSCodeObj",
        "IsMiniGame",
        "UseOwnCursor",
        "InitProgram",
        "ProcessUserEvent",
        "Pause",
        "SetPlatform",
        "onCodeObjDestruction",
    )
    missing = require_symbols(info, common_symbols + extra_symbols)
    if missing:
        errors.append("missing symbols: " + ", ".join(missing))

    if not errors:
        print(
            f"{path}: SWF 10, ABC 46, "
            f"{info.width:g}x{info.height:g}, "
            f"{info.frame_rate:g} fps, {info.frame_count} frame")
    return [f"{path}: {error}" for error in errors]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate MMVP's Fallout 4 Scaleform program SWFs.")
    parser.add_argument("browser", type=Path)
    parser.add_argument("player", type=Path)
    arguments = parser.parse_args()

    errors = []
    errors.extend(
        validate(
            arguments.browser,
            "MMVPBrowser",
            (
                "MMVPPlayer.swf",
                "MMVPBridge",
                "MMVPFontLibrary",
                "MainMenuVideoPlayer",
                "fonts_programs.swf",
                "Share-TechMono",
                "closeHolotape",
                "consumeAcceptRequest",
                "consumeNavigationRequest",
                "consumeProgressRefresh",
                "refreshMedia",
                "getMediaId",
                "getMediaLabel",
                "getMediaProgress",
                "refreshContinue",
                "getContinueChannel",
                "getContinueId",
                "getContinueLabel",
                "playMedia",
            ),
        ))
    errors.extend(
        validate(
            arguments.player,
            "MMVPPlayer",
            ("img://MMVPVideo", "UpdateState"),
        ))

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
