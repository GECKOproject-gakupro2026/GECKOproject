# Codex instructions for pretrained audio-model verification

## Scope

These instructions apply only under `pc_side/ml_pretrained_test/`.

## Objective

Complete the boardless verification stage for the pinned ST FSD50K YamNet-256
model before changing STM32 firmware or using a physical board.

## Fixed sources

- Target branch baseline: `feature/ml-voice-classification-test`
- STM32 Model Zoo commit: `1423c78953a830903485135febe1dd98ff31aed8`
- Model Zoo Services commit: `0f6210ed5156126b782e1c43249063a477484b20`
- Model SHA-256: `cd75689f072fac00d2a0fca063faec0ae0070a78d128d7f8d60c0f7ff88cd48d`
- Model size: `184240` bytes

Do not silently replace these commits or the model. If a source is unavailable,
report the failure and stop that step.

## Required order

1. Inspect installed Git, Git LFS, Python, TensorFlow/TFLite, CubeIDE, GCC,
   CubeProgrammer, and ST Edge AI versions.
2. Run `setup_pc.ps1` without dependency installation first.
3. Confirm `model_integrity.json` reports `PASS`.
4. Install the pinned Model Zoo Services dependencies only after integrity passes.
5. Run `verify_model.py --inspect-io` and save `model_io.json`.
6. Perform saved-WAV PC inference using the official Model Zoo Services
   preprocessing. Never feed raw PCM directly to the TFLite model.
7. Run ST Edge AI analyze/benchmark/generate when credentials and tools are
   available.
8. Build the official STM32U5 application without flashing it.
9. Summarize results and blockers in
   `test_results/ml_pretrained_smoke/pc_setup/report.md`.

## Safety and repository rules

- Do not flash hardware, modify option bytes, or run OTA during the boardless stage.
- Do not commit myST/OpenAI/GitHub credentials, tokens, private audio, virtual
  environments, cloned ST repositories, model binaries, or generated build trees.
- Do not mix ST Edge AI Core 2.2 generated code/runtime with Core 4.0 artifacts.
- Do not overwrite or reset dirty repositories. Stop and report the path.
- Preserve unrelated user changes.
- Use `apply_patch` for edits and show the final diff before committing.
- Record exact commands, versions, hashes, and failures; do not infer missing values.

## Verification commands

```powershell
python -m unittest discover -s pc_side/ml_pretrained_test -p "test_*.py" -v
python pc_side/ml_pretrained_test/verify_model.py <MODEL_PATH>
python pc_side/ml_pretrained_test/verify_model.py <MODEL_PATH> --inspect-io
```

The boardless stage is complete only when model integrity, TFLite I/O inspection,
ST analysis/benchmark, code generation, and an unflashed firmware build all pass.
