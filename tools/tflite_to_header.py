#!/usr/bin/env python3
"""Convert a TensorFlow Lite model into a C/C++ byte-array header.

This utility is prepared for the future classifier model. It does not generate
or embed placeholder model bytes.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def format_bytes(model: bytes, columns: int = 12) -> str:
    """Return model bytes as indented, comma-separated hexadecimal rows."""
    values = [f"0x{value:02x}" for value in model]
    return "\n".join(
        "  " + ", ".join(values[index : index + columns]) + ","
        for index in range(0, len(values), columns)
    )


def build_header(model: bytes, symbol: str) -> str:
    """Build a portable header containing the model data and its length."""
    guard = f"{symbol.upper()}_H"
    return (
        f"#ifndef {guard}\n"
        f"#define {guard}\n\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n\n"
        f"alignas(16) const uint8_t {symbol}[] = {{\n"
        f"{format_bytes(model)}\n"
        "};\n\n"
        f"const size_t {symbol}_len = sizeof({symbol});\n\n"
        f"#endif  // {guard}\n"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert a .tflite model to a C/C++ header."
    )
    parser.add_argument("input", type=Path, help="Path to the input .tflite model")
    parser.add_argument("output", type=Path, help="Path for the generated header")
    parser.add_argument(
        "--symbol",
        default="plastic_classifier_model",
        help="C array symbol (default: plastic_classifier_model)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.input.suffix.lower() != ".tflite":
        raise SystemExit("Input file must use the .tflite extension")
    if not args.input.is_file():
        raise SystemExit(f"Input model not found: {args.input}")

    model = args.input.read_bytes()
    if not model:
        raise SystemExit("Input model is empty")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(build_header(model, args.symbol), encoding="utf-8")


if __name__ == "__main__":
    main()
