# PocketVR contributor instructions

## Scope and safety
- Read `spec.md` first: it is the single source of truth for scope, status and remaining work. Apache-2.0; preserve dependency notices. No GPL/AGPL/LGPL shipped dependencies.
- Commit tested milestones. Never claim unperformed hardware acceptance.
- Choose an adequately sized local tools/build directory and set the environment variables in `docs/DEVELOPMENT.md`. Preserve user files and leave unrelated caches alone.
- Do not inspect proprietary VR implementations. Verify SDK interfaces against pinned upstream headers.

## Directory map
- `pc/common`: platform-neutral protocol, security, FEC, tracking, timing, adaptation and reassembly, shared with Android.
- `pc/driver`: Windows OpenVR provider, D3D11 compositor, encoders.
- `pc/tools`, `pc/control`, `pc/overlay`: diagnostics, desktop setup and dashboard.
- `android`: Kotlin lifecycle/UI and C++ real-time client.
- `proto`, `docs`, `third_party`, `scripts`: wire specification/vectors, decisions/acceptance, dependency lock/licenses and tooling.

## Conventions
- C++20 for shared and Windows code; Kotlin for Android UI. `/W4 /WX /permissive-` or `-Wall -Wextra -Wpedantic -Werror` on first-party targets.
- Explicit little-endian serialization, checked sizes, finite numeric validation, bounded queues/pools, authenticated session state. Never deserialize native structs from the network.
- Avoid allocations in streaming callbacks. Never block OpenVR pose/layer entry points on network or encoder work.
- Prioritize the lowest practical latency while preserving useful video bitrate/quality. Prefer fresh frames and poses, bounded queues, hardware encoding, no B-frames/lookahead, and measured adaptation. Keep hardware measurements distinct from synthetic checks.
- Record architecture choices in `docs/DECISIONS.md`. Keep `spec.md` accurate; unfinished functionality must not appear as a completed release feature.
- Tests must verify behavioral invariants, malformed input, loss, timing and security rather than mirroring implementation.

## Build and verification
- `pwsh -File scripts/build.ps1 -Configuration Debug`: configure/build Windows and run CTest plus license audit; external build output by default.
- `python scripts/license_audit.py`: verify locked first-party/third-party dependency metadata and captured license hashes.
- `cmake --preset windows-debug`, `cmake --build --preset windows-debug`, `ctest --preset windows-debug`: equivalent native commands after loading the tool and MSVC environment.
- `android/gradlew.bat -p android assembleDebug`: build the Android APK with the pinned SDK/NDK and Gradle versions.
- Use host-native shared-core tests plus Android cross-compilation locally; actual arm64 execution requires a connected phone or CI emulator and must be reported separately.
- Before committing, run applicable checks and review `git diff --check`.
