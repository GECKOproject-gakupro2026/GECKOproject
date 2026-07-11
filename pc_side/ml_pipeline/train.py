"""Audio classifier training for the B-U585I-IOT02A edge-AI firmware.

The model is intentionally small (the pipeline, not the ML content, is the
point): a 1D-CNN over one raw 2048-sample window (128 ms @ 16 kHz) - exactly
the microphone DMA buffer the firmware already keeps, so no on-device DSP is
required.

Training data, in priority order:
  1. dataset/<class_name>/*.wav   (16 kHz mono; sliced into 2048-sample windows)
  2. built-in synthetic set (silence / tone / noise) so the end-to-end
     pipeline always runs

Outputs into build/:
  model.keras, model.tflite, labels.txt, model_meta.json
"""
from __future__ import annotations

import json
import pathlib
import struct
import sys
import wave

import numpy as np

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = pathlib.Path(__file__).parent
DATASET = HERE / "dataset"
BUILD = HERE / "build"
WINDOW = 2048          # samples per inference (= firmware audio buffer)
SAMPLE_RATE = 16000
EPOCHS = 15
SYNTH_PER_CLASS = 400


def load_wav_windows(path: pathlib.Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            print(f"  skip {path.name}: need 16bit mono")
            return np.empty((0, WINDOW), np.float32)
        raw = w.readframes(w.getnframes())
    pcm = np.frombuffer(raw, np.int16).astype(np.float32) / 32768.0
    n = len(pcm) // WINDOW
    return pcm[: n * WINDOW].reshape(n, WINDOW)


def load_dataset() -> tuple[np.ndarray, np.ndarray, list[str]] | None:
    if not DATASET.is_dir():
        return None
    classes = sorted(d.name for d in DATASET.iterdir() if d.is_dir())
    if len(classes) < 2:
        return None
    xs, ys = [], []
    for idx, name in enumerate(classes):
        for wav in sorted((DATASET / name).glob("*.wav")):
            win = load_wav_windows(wav)
            xs.append(win)
            ys.append(np.full(len(win), idx, np.int32))
    x = np.concatenate(xs)
    y = np.concatenate(ys)
    print(f"dataset/: {len(classes)} classes, {len(x)} windows")
    return x, y, classes


def synthetic_dataset() -> tuple[np.ndarray, np.ndarray, list[str]]:
    rng = np.random.default_rng(42)
    t = np.arange(WINDOW, dtype=np.float32) / SAMPLE_RATE
    xs, ys = [], []
    for i in range(SYNTH_PER_CLASS):
        xs.append(rng.normal(0, 0.002, WINDOW).astype(np.float32))   # silence
        ys.append(0)
        freq = rng.uniform(200, 3000)
        amp = rng.uniform(0.1, 0.8)
        tone = amp * np.sin(2 * np.pi * freq * t + rng.uniform(0, 6.28))
        xs.append((tone + rng.normal(0, 0.01, WINDOW)).astype(np.float32))  # tone
        ys.append(1)
        xs.append(rng.normal(0, rng.uniform(0.05, 0.4), WINDOW).astype(np.float32))  # noise
        ys.append(2)
    print(f"synthetic dataset: 3 classes, {len(xs)} windows")
    return np.stack(xs), np.array(ys, np.int32), ["silence", "tone", "noise"]


def build_model(n_classes: int):
    import tensorflow as tf
    from tensorflow.keras import layers

    return tf.keras.Sequential([
        layers.Input(shape=(WINDOW, 1), name="pcm"),
        layers.Conv1D(8, 64, strides=8, activation="relu"),
        layers.Conv1D(16, 8, strides=4, activation="relu"),
        layers.Conv1D(32, 4, strides=2, activation="relu"),
        layers.GlobalAveragePooling1D(),
        layers.Dense(16, activation="relu"),
        layers.Dense(n_classes, activation="softmax", name="probs"),
    ])


def main() -> None:
    import tensorflow as tf

    data = load_dataset() or synthetic_dataset()
    x, y, classes = data
    x = x[..., None]  # (N, WINDOW, 1)
    idx = np.random.default_rng(0).permutation(len(x))
    x, y = x[idx], y[idx]

    model = build_model(len(classes))
    model.compile("adam", "sparse_categorical_crossentropy", metrics=["accuracy"])
    model.summary()
    model.fit(x, y, validation_split=0.2, epochs=EPOCHS, batch_size=64, verbose=2)

    BUILD.mkdir(exist_ok=True)
    model.save(BUILD / "model.keras")

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    tflite = converter.convert()
    (BUILD / "model.tflite").write_bytes(tflite)
    (BUILD / "labels.txt").write_text("\n".join(classes), encoding="utf-8")
    (BUILD / "model_meta.json").write_text(json.dumps({
        "window": WINDOW,
        "sample_rate": SAMPLE_RATE,
        "classes": classes,
    }, indent=2), encoding="utf-8")
    print(f"saved: model.tflite ({len(tflite)} bytes), classes={classes}")


if __name__ == "__main__":
    main()
