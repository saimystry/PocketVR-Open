# Third-party notices

Dependency versions and immutable revisions are in `third_party/dependencies.lock.json`. Original license texts below are retained verbatim. No proprietary VR source was inspected or copied.

| Component | License | License text |
|---|---|---|
| libsodium | ISC | [libsodium.txt](third_party/licenses/libsodium.txt) |
| gtest | BSD-3-Clause | [gtest.txt](third_party/licenses/gtest.txt) |
| nlohmann-json | MIT | [nlohmann-json.txt](third_party/licenses/nlohmann-json.txt) |
| spdlog | MIT | [spdlog.txt](third_party/licenses/spdlog.txt) |
| imgui | MIT | [imgui.txt](third_party/licenses/imgui.txt) |
| WebView2 SDK loader | BSD-3-Clause | [webview2-packaged.txt](third_party/licenses/webview2-packaged.txt) |
| WIL | MIT | [wil-packaged.txt](third_party/licenses/wil-packaged.txt) |
| openvr | BSD-3-Clause | [openvr.txt](third_party/licenses/openvr.txt) |
| amf | MIT | [amf.txt](third_party/licenses/amf.txt) |
| nvenc | MIT | [nvenc.txt](third_party/licenses/nvenc.txt) |
| cardboard | Apache-2.0 | [cardboard.txt](third_party/licenses/cardboard.txt) |
| Nayuki QR Code generator (desktop) | MIT | [qrcodegen.txt](third_party/licenses/qrcodegen.txt); immutable source and hashes in dependencies.lock.json |
| ZXing core 3.5.3 (phone setup scanner) | Apache-2.0 | [zxing-notice.txt](third_party/licenses/zxing-notice.txt), root LICENSE |
| Kotlin JVM standard library | Apache-2.0, BSD-3-Clause, BSL-1.0 | [kotlin-stdlib-notice.txt](third_party/licenses/kotlin-stdlib-notice.txt), captured Kotlin/Guava/GWT/ThreeTenBP/Boost texts in third_party/licenses/kotlin-* |
| JetBrains annotations | Apache-2.0 | [annotations-notice.txt](third_party/licenses/annotations-notice.txt), root LICENSE |
| vcpkg-cmake (build-only) | MIT | [vcpkg-cmake.txt](third_party/licenses/vcpkg-cmake.txt) |
| vcpkg-cmake-config (build-only) | MIT | [vcpkg-cmake-config.txt](third_party/licenses/vcpkg-cmake-config.txt) |
| vcpkg-cmake-get-vars (build-only) | MIT | [vcpkg-cmake-get-vars.txt](third_party/licenses/vcpkg-cmake-get-vars.txt) |
| vcpkg-make (build-only) | MIT | [vcpkg-make.txt](third_party/licenses/vcpkg-make.txt) |
| vcpkg-msbuild (build-only) | MIT | [vcpkg-msbuild.txt](third_party/licenses/vcpkg-msbuild.txt) |
| pkgconf (build-only) | ISC | [pkgconf-COPYING](third_party/licenses/pkgconf-COPYING) |
| vcpkg-tool-meson (build-only) | Apache-2.0 AND MIT | [vcpkg-tool-meson-COPYING](third_party/licenses/vcpkg-tool-meson-COPYING), [vcpkg-cmake.txt](third_party/licenses/vcpkg-cmake.txt) |
| vcpkg-pkgconfig-get-modules (build-only) | MIT | [vcpkg-pkgconfig-get-modules-LICENSE.txt](third_party/licenses/vcpkg-pkgconfig-get-modules-LICENSE.txt) |

OpenVR driver declarations and SDK headers, AMF headers and Cardboard integration are used under their captured upstream licenses. First-party algorithms are independently implemented. Further notices are added when additional components are integrated.

Windows/Android SDKs, MSVC, the JDK, Python, vcpkg, CMake and Ninja are development tools, not bundled product components. Their own distribution terms apply to the installed tools. The Windows VC runtime is an externally installed prerequisite; PocketVR does not redistribute it.

The Windows installer includes unmodified Android Debug Bridge binaries (`adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`) from Android SDK Platform-Tools, for USB setup and installing the phone app. Their licence notices ship beside them as `adb/NOTICE.txt`.

The Windows installer/uninstaller uses the unmodified Inno Setup 6.7.4 bootstrap, copyright (C) 1997-2026 Jordan Russell and portions copyright (C) 2000-2026 Martijn Laan, https://jrsoftware.org/. Its permissive four-condition license is captured in [inno-setup.txt](third_party/licenses/inno-setup.txt); source revision and binary/compiler hashes are pinned. It is made using **RemObjects Pascal Script**, by Carlo Kok and RemObjects Software, LLC, https://www.remobjects.com/ps.aspx, under its captured [permissive attribution license](third_party/licenses/inno-pascalscript.txt). The compiler's optional 7-Zip extraction DLLs are not shipped, and the installer script uses no archive extraction feature. LZMA decoding within the bootstrap is Igor Pavlov's public-domain SDK code; see the [captured notice](third_party/licenses/inno-lzma-notice.txt). Existing bootstrap copyright/about information is retained. See [upstream security/component notes](https://jrsoftware.org/is6help/topic_securitymeasures.htm).

Android runtime JARs, POMs and source archives are SHA-256 pinned in `third_party/android-runtime.lock.json`. Kotlin's JVM runtime incorporates permissive GWT/Guava (Apache-2.0), ThreeTenBP (BSD-3-Clause), and Boost math (BSL-1.0) code; the compiler's unrelated third-party dependencies are not runtime APK dependencies. Gradle/AGP are Apache-2.0 development tools. The vcpkg Android build uses MSYS2 Autoconf/Automake/libtool/m4/Perl and their support packages under their own development-tool terms, including GNU licenses; these tools are never packaged or linked into PocketVR. No development-tool exception authorizes a copyleft runtime dependency.

The exact installed package notices, including transitive components, are also preserved as `third_party/licenses/*-packaged.txt` and checksum-locked. Dear ImGui's embedded ProggyClean (Tristan Grimmer) and ProggyForever (Disco Hello / Tristan Grimmer) font notices are included in `imgui-packaged.txt`; both are MIT licensed. The vcpkg helper scripts accompanying Meson are MIT and Meson fragments are Apache-2.0, as stated in their combined package notice.

QR setup vendors the pinned Cardboard DeviceParams schema with its original header and [profile notice](third_party/licenses/cardboard-profile-notice.txt). A bounded independent wire decoder maps supported profiles to PocketVR geometry. No full Cardboard/Unity runtime is linked. Nayuki source is unmodified and hash-checked; ZXing is a pure Java setup dependency, with all runtime artifacts and notices audited.
