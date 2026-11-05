# Development on another Windows PC

Read `spec.md` first. Allocate ample local storage for Visual Studio, the Android SDK and NDK, dependencies and builds.

## Prerequisites and pins

- Git, Python 3, PowerShell 7; Visual Studio 2022 C++ Build Tools (v143) and Windows SDK 10.0.22621 or newer. Tested with MSVC 14.44.
- CMake **4.4.5** (the Android project requests that version), Ninja, JDK 17.
- vcpkg checked out at `9a5dfef5d1a0438a7d24856a85bd60f309d355e9`; bootstrap with metrics disabled. Dependencies are in the checked-in manifests and lock files.
- Android SDK platform 34, build-tools **36.0.0**, platform-tools, and NDK **26.3.11579264**. Install through official Android SDK tools and accept their terms yourself.
- Gradle **9.8.2**, AGP **9.5.0**, built-in Kotlin **2.2.10**. The wrapper/distribution checksums and runtime artifact hashes are checked in. Use JDK 17.
- Inno Setup **6.7.4** is pinned for the Windows installer. See [INSTALLER.md](INSTALLER.md) for Release packaging, prerequisites and signing.

Open a PowerShell terminal, adapt these example paths to available local storage, and ensure CMake/Ninja/JDK/Python/Git are on PATH:

```powershell
$env:POCKETVR_BUILD_ROOT = 'D:\PocketVR-builds'
$env:VCPKG_ROOT = 'D:\Tools\vcpkg'
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_DOWNLOADS = 'D:\Tools\vcpkg-downloads'
$env:VCPKG_BINARY_SOURCES = 'clear;files,D:\Tools\vcpkg-cache,readwrite'
$env:ANDROID_HOME = 'D:\Tools\android-sdk'
$env:ANDROID_NDK_HOME = "$env:ANDROID_HOME\ndk\26.3.11579264"
$env:JAVA_HOME = 'D:\Tools\jdk-17'
$env:GRADLE_USER_HOME = 'D:\Tools\gradle-cache'
```

`POCKETVR_DEBUG_KEYSTORE` is optional and names a debug keystore outside the repository, using the standard Android debug credentials (store and key password `android`, alias `androiddebugkey`). Android only updates an app signed by the same key, so copy this one file to every development PC; without it each PC signs with its own key and installing over the phone's app fails. It protects nothing beyond development installs and is not a store key. Release APKs are signed with it when it is set, so the installer's optimized app can update a development install; without it they stay unsigned. To keep the data of an app already on a phone, point this at the keystore that signed that app (usually `%USERPROFILE%\.android\debug.keystore` on the PC that built it). Using a different key requires one uninstall, which loses that phone's pairing data.

These are examples, not requirements to create D:. Use real paths with enough free space. Source/build paths without spaces reduce Android toolchain friction. Keep build outputs outside Git. Never commit `android/local.properties`, signing keys, identity stores or tokens.

Home-PC setup (2026-11-02): the tools are installed under `D:\Tools`, Windows SDK under `D:\Windows Kits\10`, and builds under `D:\PocketVR-builds`. Existing PowerShell processes can load the configured environment with `. D:\Tools\Enter-PocketVRTools.ps1`; new processes also have saved user environment variables and tool PATH entries. The local inventory is `D:\Tools\README-PocketVR.md`. Use the installed SDK Manager at `D:\Tools\android-sdk\cmdline-tools\19.0\bin\sdkmanager.bat` for the documented commands; the newest Android CLI has a different command interface. These paths describe this machine, not requirements for another PC.

## Windows

```powershell
pwsh -File scripts/build.ps1 -Configuration Debug
```

The script detects MSVC, fetches checksum-locked SDK headers, installs the pinned vcpkg graph, builds, runs CTest and audits dependencies. Release uses `-Configuration Release`. CI exists as a manual Windows workflow and has not run remotely. Common source cross-compilation alone is not an Android link or execution test.

## Android

After installing the prerequisites and configuring `local.properties` below, use the portable helper:

```powershell
pwsh -File scripts/build_android.ps1 -GradleExecutable D:\Tools\gradle-9.8.2\bin\gradle.bat
```

It builds native dependencies into the external build root, builds app/test APKs, audits installed and packaged dependencies, and checks 16 KB ZIP alignment. Use `-SkipDependencyInstall` only when the pinned native graph is already installed. `-Configuration Release` builds/audits the unsigned release APK; `-RunDeviceTests` additionally installs and actually executes native tests on exactly one authorized Android device. Normal build checks do not install anything on a phone.

Create ignored `android/local.properties` with Java-properties-compatible paths, for example:

```properties
sdk.dir=D\:\\Tools\\android-sdk
cmake.dir=D\:\\Tools\\cmake-4.4.5-windows-x86_64
```

Set up the pinned Gradle distribution and ARM64 dependencies (run from the repository root):

```powershell
python scripts/setup_android_gradle.py --tools D:\Tools
& "$env:VCPKG_ROOT\vcpkg.exe" install `
  --triplet arm64-pocketvr-android `
  --overlay-triplets="$PWD\android\native-deps\triplets" `
  --x-manifest-root="$PWD\android\native-deps" `
  --x-install-root="$env:POCKETVR_BUILD_ROOT\android-vcpkg-installed"
& D:\Tools\gradle-9.8.2\bin\gradle.bat -p android `
  :app:assembleDebug :app:assembleDebugAndroidTest :app:exportRuntimeArtifacts `
  -PnativeTests=true --no-daemon `
  --project-cache-dir "$env:POCKETVR_BUILD_ROOT\android\gradle-project-cache"
python scripts/audit_android.py `
  --runtime "$env:POCKETVR_BUILD_ROOT/android/app/reports/runtime-artifacts.tsv" `
  --installed "$env:POCKETVR_BUILD_ROOT/android-vcpkg-installed" `
  --apk "$env:POCKETVR_BUILD_ROOT/android/app/outputs/apk/debug/app-debug.apk"
& "$env:ANDROID_HOME\build-tools\36.0.0\zipalign.exe" -c -P 16 -v 4 `
  "$env:POCKETVR_BUILD_ROOT\android\app\outputs\apk\debug\app-debug.apk"
```

Building the instrumentation APK only compiles/packages tests. With an actual ARM64 Android API29+ device, inspect `adb devices`, then install the app and test APK and run:

```powershell
adb install -r "$env:POCKETVR_BUILD_ROOT\android\app\outputs\apk\debug\app-debug.apk"
adb install -r "$env:POCKETVR_BUILD_ROOT\android\app\outputs\apk\androidTest\debug\app-debug-androidTest.apk"
adb shell am instrument -w -r com.pocketvr.test/com.pocketvr.test.NativeInstrumentation
```

Check `native_failures` and the instrumentation exit/result; a successful install is not a test pass. The app now contains the native sensor and hardware-decoder preview paths, but still lacks the lens/timewarp renderer, so a compiled APK is not a working headset client.

## CI and device execution

The repository workflow is manual-only. Its Windows job builds/tests the driver. The Android hosted job installs checksum-pinned CMake/Gradle plus the specified SDK/NDK, builds app/test APKs and audits their actual artifacts. These jobs have been configured locally; no hosted execution is claimed. Selecting `android_device_tests` additionally uses a self-hosted Windows runner labelled `pocketvr-android-arm64`, preconfigured with the environment above, `local.properties`, JDK/CMake/Gradle/Ninja and exactly one authorized ARM64 API29+ phone. The device job builds and executes the instrumentation suite; it does not substitute host cross-compilation for phone execution. No self-hosted runner is configured. Home-PC native instrumentation was executed directly on the connected Pixel 4a (Android 11/API30) and passed on 2026-11-02; sensor, decoder and headset acceptance are separate checks.

## Signing a release APK

Android will not install an unsigned APK. Create your own key and keep it out of the repository:

```powershell
$env:POCKETVR_RELEASE_KEYSTORE = 'C:\path\to\pocketvr-release.keystore'
$env:POCKETVR_RELEASE_PASSWORD = '<your password>'
pwsh -File scripts/build_android.ps1 -Configuration Release
```

The key alias must be `pocketvr`. An app can only update over one signed with the same key.
