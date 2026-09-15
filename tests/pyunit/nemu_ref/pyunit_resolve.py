import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from util.nemu_ref import resolve


class NemuReferenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = resolve.ref_path("normal", self.root)
        self.path.parent.mkdir(parents=True)
        self.path.write_bytes(b"test reference")
        self.config = Path(str(self.path) + ".config")
        self.config.write_text("CONFIG_DIFFTEST_CHECK_FCSR=y\n")
        self.manifest = {
            "commit": resolve.LOCK["commit"],
            "release": resolve.LOCK["release"],
            "variant": "normal", "register_size": 1376,
            "dependencies": resolve.LOCK["dependencies"],
            "lock_sha256": self.digest(resolve.HERE / "lock.json"),
            "sha256": self.digest(self.path),
            "config_sha256": self.digest(self.config),
            "fragment_sha256": {
                name: self.digest(resolve.HERE / name)
                for name in ("common.config", "scalar.config", "normal.config")
            },
        }
        self.save_manifest()

    @staticmethod
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def save_manifest(self):
        (self.path.parent / "manifest.json").write_text(
            json.dumps(self.manifest)
        )

    def test_verified_path(self):
        self.assertEqual(resolve.verify_ref("normal", self.root), self.path)

    def test_different_variants_share_release(self):
        for variant in resolve.LOCK["variants"]:
            path = resolve.ref_path(variant, self.root)
            self.assertEqual(path.parent.parent, self.path.parent.parent)

    def test_unknown_variant(self):
        with self.assertRaises(ValueError):
            resolve.ref_path("legacy", self.root)

    def test_reject_modified_binary(self):
        self.path.write_bytes(b"different reference")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            resolve.verify_ref("normal", self.root)

    def test_reject_modified_config(self):
        self.config.write_text("# CONFIG_DIFFTEST_CHECK_FCSR is not set\n")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            resolve.verify_ref("normal", self.root)

    def test_reject_wrong_commit_or_layout(self):
        for key, value in (("commit", "old"), ("register_size", 1368)):
            with self.subTest(key=key):
                old = self.manifest[key]
                self.manifest[key] = value
                self.save_manifest()
                with self.assertRaisesRegex(ValueError, "manifest mismatch"):
                    resolve.verify_ref("normal", self.root)
                self.manifest[key] = old

    def test_reject_stale_fragment(self):
        self.manifest["fragment_sha256"]["common.config"] = "old"
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "fragment mismatch"):
            resolve.verify_ref("normal", self.root)
