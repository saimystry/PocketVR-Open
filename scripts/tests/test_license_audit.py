import importlib.util
from pathlib import Path
import unittest
import tempfile
import json
import hashlib

spec = importlib.util.spec_from_file_location("license_audit", Path(__file__).parents[1] / "license_audit.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class LicenseExpressions(unittest.TestCase):
    def test_permissive_combinations(self):
        for expression in ["MIT", "(MIT OR Apache-2.0) AND BSD-3-Clause", "ISC", "BSL-1.0", "LicenseRef-Inno-Setup AND LicenseRef-RemObjects-Pascal-Script AND LicenseRef-LZMA-PublicDomain"]:
            self.assertTrue(audit.allowed_expression(expression), expression)
        self.assertFalse(audit.allowed_expression('LicenseRef-Unreviewed-Installer'))

    def test_reject_copyleft_unknown_and_malformed(self):
        for expression in ["", "GPL-2.0-only", "LGPL-2.1-only", "AGPL-3.0-only", "MIT OR GPL-3.0-only", "NONE", "NOASSERTION", "MIT WITH LLVM-exception", "MIT garbage", "(MIT", "MIT OR", "MIT)", "MIT;Apache-2.0", "AND MIT", "()"]:
            self.assertFalse(audit.allowed_expression(expression), expression)


class InstalledPackageAudit(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "third_party").mkdir()
        (self.root / "LICENSE").write_text("Apache-2.0")
        (self.root / "NOTICE").write_text("PocketVR")
        (self.root / "license.txt").write_bytes(b"Reviewed license")
        digest = hashlib.sha256(b"Reviewed license").hexdigest()
        lock = {"vcpkg_baseline": "b"*40, "dependencies": [{"name": "alpha", "version": "1.0.0", "port_version": 2, "license": "MIT", "revision": "a"*40, "installed_license_sha256": digest, "license_files": [{"path": "license.txt", "sha256": digest}]}]}
        (self.root / "third_party/dependencies.lock.json").write_text(json.dumps(lock))
        (self.root / "vcpkg.json").write_text(json.dumps({"builtin-baseline": "b"*40, "dependencies": ["alpha"]}))
        self.installed = self.root / "installed"
        (self.installed / "vcpkg").mkdir(parents=True)
        (self.installed / "x64/share/alpha").mkdir(parents=True)
        (self.installed / "x64/share/alpha/copyright").write_bytes(b"Reviewed license")
        self.status = self.installed / "vcpkg/status"
        self.status.write_text("Package: alpha\nVersion: 1.0.0\nPort-Version: 2\nArchitecture: x64\nStatus: install ok installed\n\n")

    def test_vendored_source_and_revision_are_enforced(self):
        path = self.root / "third_party/dependencies.lock.json"
        lock = json.loads(path.read_text())
        dep = lock["dependencies"][0]
        dep["repository"] = "https://github.com/example/alpha"
        source = self.root / "source.cpp"
        source.write_bytes(b"reviewed source")
        item = {"path": "source.cpp", "url": "https://raw.githubusercontent.com/example/alpha/" + "a"*40 + "/source.cpp",
                "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
        dep["vendored_files"] = [item]
        path.write_text(json.dumps(lock))
        self.assertEqual(audit.audit(self.root), [])
        source.write_bytes(b"changed source")
        self.assertTrue(any("vendored source" in error for error in audit.audit(self.root)))
        item["sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        item["url"] = item["url"].replace("a"*40, "b"*40)
        path.write_text(json.dumps(lock))
        self.assertTrue(any("vendored source" in error for error in audit.audit(self.root)))

    def test_verified_package_and_port_version(self):
        self.assertEqual(audit.audit(self.root, self.installed), [])

    def test_unreviewed_transitive_package_is_rejected(self):
        with self.status.open("a") as file:
            file.write("Package: unknown\nVersion: 1\nArchitecture: x64\nStatus: install ok installed\n\n")
        self.assertTrue(any("Unreviewed installed" in error for error in audit.audit(self.root, self.installed)))

    def test_missing_notice_and_changed_version_are_rejected(self):
        (self.installed / "x64/share/alpha/copyright").unlink()
        self.status.write_text(self.status.read_text().replace("1.0.0", "2.0.0"))
        errors = audit.audit(self.root, self.installed)
        self.assertTrue(any("copyright" in error for error in errors))
        self.assertTrue(any("version" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
