import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from verify_model import VerificationError, is_git_lfs_pointer, verify_integrity

MANIFEST = Path(__file__).with_name("model_manifest.json")


class VerifyModelTests(unittest.TestCase):
    def test_manifest_uses_st_training_label_order(self):
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(
            manifest["output"]["labels"],
            [
                "Crying_and_sobbing",
                "Glass",
                "Gunshot_and_gunfire",
                "Knock",
                "Speech",
                "other",
            ],
        )

    def test_accepts_matching_artifact(self) -> None:
        payload = b"TFL3-test-model"
        manifest = {
            "model": {
                "size_bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        }
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.tflite"
            model.write_bytes(payload)
            result = verify_integrity(model, manifest)
        self.assertEqual(result["size_bytes"], len(payload))

    def test_rejects_lfs_pointer(self) -> None:
        pointer = (
            b"version https://git-lfs.github.com/spec/v1\n"
            b"oid sha256:abc\nsize 1\n"
        )
        manifest = {"model": {"size_bytes": len(pointer), "sha256": "unused"}}
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.tflite"
            model.write_bytes(pointer)
            self.assertTrue(is_git_lfs_pointer(model))
            with self.assertRaisesRegex(VerificationError, "Git LFS pointer"):
                verify_integrity(model, manifest)

    def test_rejects_wrong_size(self) -> None:
        manifest = {"model": {"size_bytes": 99, "sha256": "unused"}}
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.tflite"
            model.write_bytes(b"short")
            with self.assertRaisesRegex(VerificationError, "size mismatch"):
                verify_integrity(model, manifest)


if __name__ == "__main__":
    unittest.main()
