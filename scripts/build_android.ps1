param(
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [string]$GradleExecutable,
    [switch]$SkipDependencyInstall,
    [switch]$RunDeviceTests
)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
# Mirror scripts/build.ps1: an explicit tool environment must win over an
# inherited POCKETVR_BUILD_ROOT, otherwise the app can be built into a
# different source tree's output directory.
$toolEnvironment = $env:POCKETVR_TOOL_ENV
if ($toolEnvironment) {
    if (-not (Test-Path -LiteralPath $toolEnvironment -PathType Leaf)) { throw 'POCKETVR_TOOL_ENV must identify an existing environment script.' }
    . $toolEnvironment
}
foreach ($name in @('ANDROID_HOME','VCPKG_ROOT','POCKETVR_BUILD_ROOT')) {
    if (-not [Environment]::GetEnvironmentVariable($name)) { throw "Set $name before building; see docs/DEVELOPMENT.md." }
}
# vcpkg and Gradle must use the same pinned NDK, even on hosted images that
# export ANDROID_NDK_HOME pointing at a newer preinstalled release.
$env:ANDROID_NDK_HOME=Join-Path $env:ANDROID_HOME 'ndk/26.3.11579264'
if (-not $env:GRADLE_USER_HOME) { $env:GRADLE_USER_HOME=Join-Path $env:POCKETVR_BUILD_ROOT 'gradle-cache' }
if (-not $env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS=Join-Path $env:POCKETVR_BUILD_ROOT 'downloads' }
if (-not (Test-Path -LiteralPath $env:ANDROID_NDK_HOME)) { throw 'The pinned Android NDK is not installed.' }
if ($RunDeviceTests -and $Configuration -ne 'Debug') { throw 'Native instrumentation requires the Debug build.' }
if (-not $GradleExecutable) {
    $GradleExecutable=Join-Path $repoRoot 'android/gradlew.bat'
}
$nativeInstall=Join-Path $env:POCKETVR_BUILD_ROOT 'android-vcpkg-installed'
$appBuild=Join-Path $env:POCKETVR_BUILD_ROOT 'android/app'
Push-Location $repoRoot
try {
    if (-not $SkipDependencyInstall) {
        & "$env:VCPKG_ROOT/vcpkg.exe" install --triplet arm64-pocketvr-android `
            "--overlay-triplets=$repoRoot/android/native-deps/triplets" `
            "--x-manifest-root=$repoRoot/android/native-deps" "--x-install-root=$nativeInstall" `
            "--x-buildtrees-root=$env:POCKETVR_BUILD_ROOT/android-vcpkg-buildtrees" `
            "--x-packages-root=$env:POCKETVR_BUILD_ROOT/android-vcpkg-packages"
        if ($LASTEXITCODE) { throw 'Android native dependency build failed.' }
    }
    $tasks=@(":app:assemble$Configuration", ':app:exportRuntimeArtifacts')
    if ($Configuration -eq 'Debug') { $tasks+= ':app:assembleDebugAndroidTest' }
    & $GradleExecutable -p android @tasks "-PnativeTests=$($Configuration -eq 'Debug')" `
        --no-daemon --project-cache-dir "$env:POCKETVR_BUILD_ROOT/android/gradle-project-cache"
    if ($LASTEXITCODE) { throw 'Android Gradle build failed.' }
    $variant=$Configuration.ToLowerInvariant()
    $apk=Join-Path $appBuild "outputs/apk/$variant/app-$variant$(if ($Configuration -eq 'Release' -and -not $env:POCKETVR_DEBUG_KEYSTORE -and -not $env:POCKETVR_RELEASE_KEYSTORE) {'-unsigned'}).apk"
    & python scripts/audit_android.py --runtime "$appBuild/reports/runtime-artifacts.tsv" --installed $nativeInstall --apk $apk
    if ($LASTEXITCODE) { throw 'Android artifact/license audit failed.' }
    $androidTools=Get-Content third_party/android-tools.lock.json -Raw | ConvertFrom-Json
    & "$env:ANDROID_HOME/build-tools/$($androidTools.build_tools)/zipalign.exe" -c -P 16 4 $apk
    if ($LASTEXITCODE) { throw 'Android 16 KB APK alignment check failed.' }
    if ($RunDeviceTests) {
        $adb=Join-Path $env:ANDROID_HOME 'platform-tools/adb.exe'
        $devices=@(& $adb devices | Select-String '^\S+\s+device$')
        if ($devices.Count -ne 1) { throw 'Connect exactly one authorized ARM64 Android device for instrumentation.' }
        & $adb install -r $apk
        if ($LASTEXITCODE) { throw 'App installation failed.' }
        & $adb install -r "$appBuild/outputs/apk/androidTest/debug/app-debug-androidTest.apk"
        if ($LASTEXITCODE) { throw 'Test APK installation failed.' }
        & $adb shell input keyevent KEYCODE_WAKEUP
        $result=& $adb shell am instrument -w -r com.pocketvr.test/com.pocketvr.test.NativeInstrumentation
        $result | Write-Output
        if ($LASTEXITCODE -or -not ($result -match 'native_failures=0') -or -not ($result -match 'INSTRUMENTATION_CODE: -1')) {
            throw 'Android native instrumentation did not report a clean pass.'
        }
    }
} finally { Pop-Location }
