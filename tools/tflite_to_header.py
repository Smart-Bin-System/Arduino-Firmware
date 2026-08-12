#!/usr/bin/env python3
"""Convert an INT8 TensorFlow Lite model to an Arduino C++ header.

Usage example:
    python tools/tflite_to_header.py plastic_classifier_int8.tflite \
        "ESP32 - Codes/ESP32-S3 Smart Bin/plastic_classifier_model.h"

The input model is read without modification and emitted byte-for-byte as a
16-byte-aligned ``unsigned char`` array.
"""

from __future__ import annotations

import argparse
from pathlib import Path


MODEL_SYMBOL = "plastic_classifier_model"
BYTES_PER_LINE = 12


def format_bytes(model: bytes, columns: int = BYTES_PER_LINE) -> str:
    """Return model bytes as indented, comma-separated hexadecimal rows."""
    values = [f"0x{value:02x}" for value in model]
    return "\n".join(
        "    " + ", ".join(values[index : index + columns]) + ","
        for index in range(0, len(values), columns)
    )


def build_header(model: bytes) -> str:
    """Build the Arduino-compatible header for the classifier model."""
    return (
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"alignas(16) const unsigned char {MODEL_SYMBOL}[] = {{\n"
        f"{format_bytes(model)}\n"
        "};\n\n"
        f"const unsigned int {MODEL_SYMBOL}_len = {len(model)};\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert a .tflite model to a C/C++ header."
    )
    parser.add_argument("input", type=Path, help="Path to the input .tflite model")
    parser.add_argument("output", type=Path, help="Path for the generated header")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.input.suffix.lower() != ".tflite":
        raise SystemExit(f"Error: input file must use the .tflite extension: {args.input}")
    if not args.input.is_file():
        raise SystemExit(f"Error: input model file does not exist: {args.input}")

    model = args.input.read_bytes()
    if not model:
        raise SystemExit(f"Error: input model file is empty: {args.input}")

    try:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(build_header(model), encoding="utf-8", newline="\n")
    except OSError as error:
        raise SystemExit(f"Error: could not write output header: {error}") from error


if __name__ == "__main__":
    main()
