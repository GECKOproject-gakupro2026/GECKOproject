#!/usr/bin/env python3
"""Verify the pinned ST Model Zoo TFLite artifact and optionally inspect its I/O.

The integrity check uses only the Python standard library. I/O inspection needs
either tflite-runtime or TensorFlow; no model inference or training is performed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


class VerificationError(RuntimeError):
    """Raised when the artifact differs from the pinned manifest."""


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_git_lfs_pointer(path: Path) -> bool:
    with path.open("rb") as stream:
        return stream.read(200).startswith(b"version https://git-lfs.github.com/spec/v1")


def verify_integrity(model_path: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    if not model_path.is_file():
        raise VerificationError(f"model not found: {model_path}")
    if is_git_lfs_pointer(model_path):
        raise VerificationError("the file is a Git LFS pointer; run `git lfs pull`")

    expected = manifest["model"]
    actual_size = model_path.stat().st_size
    actual_sha256 = sha256_file(model_path)
    if actual_size != expected["size_bytes"]:
        raise VerificationError(
            f"size mismatch: expected {expected['size_bytes']}, got {actual_size}"
        )
    if actual_sha256.lower() != expected["sha256"].lower():
        raise VerificationError(
            f"sha256 mismatch: expected {expected['sha256']}, got {actual_sha256}"
        )
    return {"size_bytes": actual_size, "sha256": actual_sha256}


def _make_interpreter(model_path: Path) -> tuple[Any, str]:
    try:
        from tflite_runtime.interpreter import Interpreter

        return Interpreter(model_path=str(model_path)), "tflite-runtime"
    except ImportError:
        try:
            import tensorflow as tf

            return tf.lite.Interpreter(model_path=str(model_path)), "tensorflow"
        except ImportError as exc:
            raise VerificationError(
                "I/O inspection requires `tflite-runtime` or `tensorflow`"
            ) from exc


def _shape(detail: dict[str, Any]) -> list[int]:
    return [int(value) for value in detail["shape"]]


def _dtype_name(detail: dict[str, Any]) -> str:
    dtype = detail["dtype"]
    return str(getattr(dtype, "__name__", dtype))


def inspect_io(model_path: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    interpreter, backend = _make_interpreter(model_path)
    interpreter.allocate_tensors()
    inputs = interpreter.get_input_details()
    outputs = interpreter.get_output_details()
    if len(inputs) != 1 or len(outputs) != 1:
        raise VerificationError(
            f"expected one input and one output, got {len(inputs)} and {len(outputs)}"
        )

    actual = {
        "backend": backend,
        "input": {
            "shape": _shape(inputs[0]),
            "dtype": _dtype_name(inputs[0]),
            "quantization": list(inputs[0].get("quantization", ())),
        },
        "output": {
            "shape": _shape(outputs[0]),
            "dtype": _dtype_name(outputs[0]),
            "quantization": list(outputs[0].get("quantization", ())),
        },
    }

    for tensor_name in ("input", "output"):
        expected = manifest[tensor_name]
        for field in ("shape", "dtype"):
            if actual[tensor_name][field] != expected[field]:
                raise VerificationError(
                    f"{tensor_name} {field} mismatch: "
                    f"expected {expected[field]}, got {actual[tensor_name][field]}"
                )
    return actual


def parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path, help="path to the downloaded .tflite model")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=script_dir / "model_manifest.json",
        help="pinned model manifest",
    )
    parser.add_argument(
        "--inspect-io",
        action="store_true",
        help="inspect and validate TFLite tensors (requires an interpreter package)",
    )
    parser.add_argument("--json-out", type=Path, help="write the result as JSON")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        manifest = load_manifest(args.manifest)
        result: dict[str, Any] = {
            "model": str(args.model),
            "integrity": verify_integrity(args.model, manifest),
        }
        if args.inspect_io:
            result["io"] = inspect_io(args.model, manifest)
        result["status"] = "PASS"
    except (OSError, KeyError, json.JSONDecodeError, VerificationError) as exc:
        result = {"model": str(args.model), "status": "FAIL", "error": str(exc)}
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 1

    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
