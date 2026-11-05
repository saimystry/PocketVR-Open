import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).parents[1]))
spec = importlib.util.spec_from_file_location("android_audit", Path(__file__).parents[1] / "audit_android.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class AndroidArtifactAudit(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="PocketVR-android-audit-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        original = Path(__file__).resolve().parents[2]
        self.entries = json.loads((original / "third_party/android-runtime.lock.json").read_text())
        files = {"LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md", "third_party/android-runtime.lock.json",
                 "third_party/android-tools.lock.json", "third_party/dependencies.lock.json",
                 "android/build.gradle.kts", "android/native-deps/vcpkg.json", "android/gradlew", "android/gradlew.bat",
                 "android/gradle/wrapper/gradle-wrapper.properties", "android/gradle/wrapper/gradle-wrapper.jar",
                 "third_party/licenses/libsodium.txt", "third_party/licenses/nlohmann-json.txt",
                 "third_party/licenses/cardboard-profile-notice.txt"}
        files.update(item["path"] for entry in self.entries for item in entry["license_files"])
        for relative in files:
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original / relative, target)
        self.runtime = self.root / "runtime.tsv"
        lines = []
        for index, entry in enumerate(self.entries):
            artifact = self.root / f"fixture-{index}.jar"
            artifact.write_bytes(entry["coordinate"].encode())
            entry["sha256"] = hashlib.sha256(artifact.read_bytes()).hexdigest()
            lines.append(entry["coordinate"] + "\t" + str(artifact))
        (self.root / "third_party/android-runtime.lock.json").write_text(json.dumps(self.entries))
        self.runtime.write_text("\n".join(lines) + "\n")
        self.apk = self.root / "fixture.apk"
        notices = {"LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"}
        notices.update(Path(item["path"]).name for entry in self.entries for item in entry["license_files"])
        notices.update({"libsodium.txt", "nlohmann-json.txt", "cardboard-profile-notice.txt"})
        with zipfile.ZipFile(self.apk, "w") as archive:
            archive.writestr("lib/arm64-v8a/libpocketvr_native.so", b"audit-fixture")
            for name in notices:
                source = self.root / name if name in {"LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"} else self.root / "third_party/licenses" / name
                archive.write(source, "assets/licenses/" + name)

    def test_exact_runtime_and_notices_pass(self):
        self.assertEqual(audit.check(self.root, self.runtime, self.apk), [])

    def test_changed_wrapper_launcher_is_rejected(self):
        (self.root / "android/gradlew.bat").write_bytes(b"unreviewed launcher")
        self.assertTrue(any("wrapper script" in error for error in audit.check(self.root)))

    def test_new_transitive_dependency_and_modified_artifact_fail(self):
        with self.runtime.open("a") as stream:
            stream.write("org.unreviewed:codec:1\tunknown.jar\n")
        self.assertTrue(any("Unreviewed" in error for error in audit.check(self.root, self.runtime)))
        (self.root / "fixture-0.jar").write_bytes(b"tampered artifact")
        self.assertTrue(any(self.entries[0]["coordinate"] in error for error in audit.check(self.root, self.runtime)))

    def test_extra_native_library_and_changed_notice_fail(self):
        with zipfile.ZipFile(self.apk, "a") as archive:
            archive.writestr("lib/arm64-v8a/libunreviewed.so", b"not approved")
        errors = audit.check(self.root, self.runtime, self.apk)
        self.assertTrue(any("native libraries" in error for error in errors))
        (self.root / self.entries[0]["license_files"][0]["path"]).write_text("changed notice")
        self.assertTrue(any("notice differs" in error for error in audit.check(self.root)))
