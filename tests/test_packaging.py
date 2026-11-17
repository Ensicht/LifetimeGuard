"""Exercise installable ZIP identity, ownership and source receipts."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from package import DLL_NAME, WMP_DLL_NAME, bonesystem_files, installer_files, pack, sha, verified_build


class PackagingTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.build = self.root / "out"
        self.build.mkdir()
        self.source = self.root / "src"
        shutil.copytree(ROOT / "src/BoneSystem", self.source / "BoneSystem")
        self.dll = b"standalone fixture"
        self.wmp = b"WMP fixture"
        (self.build / DLL_NAME).write_bytes(self.dll)
        (self.build / WMP_DLL_NAME).write_bytes(self.wmp)
        dependencies = self.root / "third_party"
        dependencies.mkdir()
        (dependencies / "header.h").write_bytes(b"dependency fixture")
        manifest = {"third_party/header.h": sha(b"dependency fixture")}
        (dependencies / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.state = {
            "status": "BUILT", "version": "1.0.0", "tests_passed": True,
            "calls": [{"name": "compile", "returncode": 0}],
            "binaries": {DLL_NAME: sha(self.dll), WMP_DLL_NAME: sha(self.wmp)},
            "dependencies": manifest,
            "source": {path.relative_to(self.source).as_posix(): sha(path.read_text("utf-8-sig").encode())
                       for path in self.source.rglob("*") if path.is_file()},
        }
        self.save_receipt()
        for name in ("README.md", "LICENSE"):
            (self.root / name).write_bytes(("BoneSystem mention in " + name).encode())
        licenses = self.root / "licenses"
        licenses.mkdir()
        (licenses / "REFramework.txt").write_bytes(b"license fixture")

    def save_receipt(self):
        (self.build / "build.json").write_text(json.dumps(self.state), encoding="utf-8")

    def test_two_binaries_share_the_same_source_receipt(self):
        self.assertEqual(verified_build(self.root, self.build, require_tests=True), (self.dll, "1.0.0"))
        self.assertEqual(verified_build(self.root, self.build, wmp=True), (self.wmp, "1.0.0"))

    def test_stale_source_or_dependency_is_rejected(self):
        dependency = self.root / "third_party/header.h"
        dependency.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "Dependency changed"):
            verified_build(self.root, self.build)
        dependency.write_bytes(b"dependency fixture")
        (self.source / "BoneSystem/Lookup.lua").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "source changed"):
            verified_build(self.root, self.build)

    def test_tampered_dll_is_rejected(self):
        (self.build / WMP_DLL_NAME).write_bytes(b"not the built binary")
        with self.assertRaisesRegex(ValueError, "DLL differs"):
            verified_build(self.root, self.build, wmp=True)

    def test_incomplete_build_and_missing_release_tests(self):
        self.state["tests_passed"] = False
        self.save_receipt()
        self.assertEqual(verified_build(self.root, self.build), (self.dll, "1.0.0"))
        with self.assertRaisesRegex(ValueError, "test suite"):
            verified_build(self.root, self.build, require_tests=True)
        self.state["status"] = "BUILDING"
        self.save_receipt()
        with self.assertRaisesRegex(ValueError, "finish"):
            verified_build(self.root, self.build)

    def test_two_variants_share_lookup_and_own_disjoint_paths(self):
        independent = bonesystem_files(self.root, "1.0.0")
        bundled = bonesystem_files(self.root, "1.0.0", wmp=True)
        self.assertFalse({p.casefold() for p in independent} & {p.casefold() for p in bundled})
        self.assertEqual(independent["reframework/autorun/LifetimeGuard/BoneSystem/Lookup.lua"],
                         bundled["reframework/autorun/WMPOptimizationPatches/BoneSystem/Lookup.lua"])

    def test_installer_rebuild_and_user_owned_readme(self):
        files = installer_files(self.root, self.dll, "1.0.0")
        self.assertEqual(files["README.md"], (self.root / "README.md").read_bytes())
        info = dict(line.split("=", 1) for line in files["modinfo.ini"].decode().splitlines())
        self.assertEqual(info["name"], "LifetimeGuard v1.0.0")
        self.assertEqual(info["version"], "1.0.0")
        self.assertEqual(info["NameAsBundle"], "DirectStorage File Lifetime Guard v0.1")
        self.assertIn("licenses/REFramework.txt", files)
        self.assertIn("reframework/plugins/" + DLL_NAME, files)
        self.assertFalse(any("BoneSystemLookupFix" in path or "/data/" in path for path in files))
        a, b = self.root / "a.zip", self.root / "b.zip"
        first, second = pack(a, files), pack(b, dict(reversed(list(files.items()))))
        self.assertEqual(first["sha256"], second["sha256"])
        with ZipFile(a) as archive:
            self.assertEqual({name: archive.read(name) for name in archive.namelist()}, files)


if __name__ == "__main__":
    unittest.main()
