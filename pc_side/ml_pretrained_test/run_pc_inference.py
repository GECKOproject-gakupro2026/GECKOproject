#!/usr/bin/env python3
"""Run the pinned ST FSD50K YamNet model on saved WAV files.

The feature extraction and input quantization match the pinned STM32 Model Zoo
Services implementation. This script is a boardless smoke test, not an accuracy
benchmark.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import librosa
import numpy as np
import tensorflow as tf


def load_manifest(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def make_patches(wave_path: Path) -> np.ndarray:
    wave, _ = librosa.load(wave_path, sr=16_000, mono=True)
    intervals = librosa.effects.split(
        wave, top_db=60, frame_length=3_200, hop_length=3_200
    )
    if not len(intervals):
        raise ValueError(f"no non-silent interval found: {wave_path}")
    wave = np.concatenate([wave[start:end] for start, end in intervals])
    wave = wave[: 10 * 16_000]
    if len(wave) < 16_000:
        repeats = 16_000 // len(wave)
        wave = np.tile(wave, repeats + 1)[:16_000]

    mel = librosa.feature.melspectrogram(
        y=wave,
        sr=16_000,
        n_fft=512,
        hop_length=160,
        win_length=400,
        window="hann",
        center=False,
        pad_mode="constant",
        power=1.0,
        n_mels=64,
        fmin=125,
        fmax=7_500,
        norm=None,
        htk=True,
    )
    mel = np.log(mel + 1e-6)
    framed = librosa.util.frame(mel, frame_length=96, hop_length=72)
    return np.transpose(framed, axes=(2, 0, 1))


def infer(model_path: Path, labels: list[str], wave_paths: list[Path]) -> list[dict]:
    interpreter = tf.lite.Interpreter(model_path=str(model_path), num_threads=1)
    interpreter.allocate_tensors()
    input_info = interpreter.get_input_details()[0]
    output_info = interpreter.get_output_details()[0]
    scale, zero_point = input_info["quantization"]
    if scale <= 0:
        raise ValueError("model input has no valid quantization scale")

    results = []
    for wave_path in wave_paths:
        patches = make_patches(wave_path)
        patch_scores = []
        for patch in patches:
            quantized = np.clip(
                np.round(patch / scale + zero_point), -128, 127
            ).astype(np.int8)[None, ..., None]
            interpreter.set_tensor(input_info["index"], quantized)
            interpreter.invoke()
            patch_scores.append(interpreter.get_tensor(output_info["index"])[0])

        mean_scores = np.asarray(patch_scores).mean(axis=0)
        top_index = int(np.argmax(mean_scores))
        results.append(
            {
                "file": str(wave_path),
                "patches": int(len(patches)),
                "top_index": top_index,
                "top_label": labels[top_index],
                "top_score": float(mean_scores[top_index]),
                "speech_score": float(mean_scores[labels.index("Speech")]),
                "mean_scores": {
                    label: float(mean_scores[index])
                    for index, label in enumerate(labels)
                },
            }
        )
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("wav", nargs="+", type=Path)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name("model_manifest.json"),
    )
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    labels = manifest["output"]["labels"]
    results = infer(args.model, labels, args.wav)
    rendered = json.dumps(results, indent=2, ensure_ascii=False)
    print(rendered)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
