plugins { id("com.android.application") }

val externalRoot = providers.environmentVariable("POCKETVR_BUILD_ROOT")
    .orElse(rootProject.layout.projectDirectory.dir("../build").asFile.absolutePath).get()
layout.buildDirectory.set(file("$externalRoot/android/app"))
val nativeInstall = "$externalRoot/android-vcpkg-installed"
// One debug key shared by every development PC, so an APK built on any of them
// installs over the phone's app and keeps its pairing data. See docs/DEVELOPMENT.md.
val sharedDebugKeystore = providers.environmentVariable("POCKETVR_DEBUG_KEYSTORE").orNull?.let { file(it) }
if (sharedDebugKeystore != null && !sharedDebugKeystore.isFile)
    throw GradleException("POCKETVR_DEBUG_KEYSTORE does not identify a file: $sharedDebugKeystore")
// Published builds use a private key that never enters this repository. Without one,
// a release build is left unsigned unless a shared debug key is set.
val releaseKeystore = providers.environmentVariable("POCKETVR_RELEASE_KEYSTORE").orNull?.let { file(it) }
val releasePassword = providers.environmentVariable("POCKETVR_RELEASE_PASSWORD").orNull
if (releaseKeystore != null && (!releaseKeystore.isFile || releasePassword.isNullOrEmpty()))
    throw GradleException("Set POCKETVR_RELEASE_KEYSTORE to a file and POCKETVR_RELEASE_PASSWORD to its password")
android {
    namespace = "com.pocketvr"
    compileSdk = 34
    buildToolsVersion = "36.0.0"
    ndkVersion = "26.3.11579264"
    defaultConfig {
        buildConfigField("boolean", "NATIVE_TESTS", providers.gradleProperty("nativeTests").orElse("false").get().toBoolean().toString())
        applicationId = "com.pocketvr"
        manifestPlaceholders["appLabel"] = "PocketVR"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-dev"
        testInstrumentationRunner = "com.pocketvr.test.NativeInstrumentation"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DVCPKG_INSTALLED_DIR=$nativeInstall",
                    "-DPOCKETVR_NATIVE_TESTS=${if (providers.gradleProperty("nativeTests").orElse("false").get().toBoolean()) "ON" else "OFF"}")
                cppFlags += listOf("-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror")
            }
        }
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.4.5"
            buildStagingDirectory = file(providers.environmentVariable("POCKETVR_ANDROID_CXX_ROOT")
                .orElse("$externalRoot/android/cxx").get())
        }
    }
    signingConfigs {
        getByName("debug") {
            if (sharedDebugKeystore != null) {
                storeFile = sharedDebugKeystore
                storePassword = "android"
                keyAlias = "androiddebugkey"
                keyPassword = "android"
            }
        }
        create("release") {
            if (releaseKeystore != null) {
                storeFile = releaseKeystore
                storePassword = releasePassword
                keyAlias = "pocketvr"
                keyPassword = releasePassword
            }
        }
    }
    buildTypes {
        debug {
            isJniDebuggable = true
            if (providers.gradleProperty("qrVerification").orElse("false").get().toBoolean()) {
                applicationIdSuffix = ".qrcheck"
                versionNameSuffix = "-qrcheck"
                manifestPlaceholders["appLabel"] = "PocketVR QR check"
            }
        }
        release {
            // Same key as debug builds, so the installer's optimized app can
            // update a development install without losing its pairing.
            if (releaseKeystore != null) signingConfig = signingConfigs.getByName("release")
            else if (sharedDebugKeystore != null) signingConfig = signingConfigs.getByName("debug")
            buildConfigField("boolean", "NATIVE_TESTS", "false")
            isMinifyEnabled = false
            externalNativeBuild.cmake.arguments += "-DPOCKETVR_NATIVE_TESTS=OFF"
        }
    }
    buildFeatures { buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    packaging {
        jniLibs.useLegacyPackaging = false
        resources.excludes += listOf("META-INF/DEPENDENCIES")
    }
}
dependencies {
    implementation("org.jetbrains.kotlin:kotlin-stdlib:2.2.10")
    implementation("com.google.zxing:core:3.5.3")
}

val legalAssets = layout.buildDirectory.dir("generated/legal-assets")
abstract class StageNotices : DefaultTask() {
    @get:InputFiles @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val notices: ConfigurableFileCollection
    @get:OutputDirectory abstract val outputDirectory: DirectoryProperty
    @TaskAction fun stage() {
        val destination = outputDirectory.get().asFile.resolve("licenses")
        destination.mkdirs()
        notices.files.forEach { it.copyTo(destination.resolve(it.name), overwrite = true) }
    }
}
val stageNotices = tasks.register<StageNotices>("stageNotices") {
    notices.from(rootProject.file("../LICENSE"), rootProject.file("../NOTICE"), rootProject.file("../THIRD_PARTY_NOTICES.md"))
    notices.from(rootProject.fileTree("../third_party/licenses") {
        include("libsodium.txt", "nlohmann-json.txt", "gtest.txt", "kotlin-*", "annotations-notice.txt", "zxing-notice.txt", "cardboard-profile-notice.txt")
    })
    outputDirectory.set(legalAssets)
}
androidComponents.onVariants { variant ->
    variant.sources.assets?.addGeneratedSourceDirectory(stageNotices) { it.outputDirectory }
}

tasks.register("exportRuntimeArtifacts") {
    doLast {
        val output = layout.buildDirectory.file("reports/runtime-artifacts.tsv").get().asFile
        output.parentFile.mkdirs()
        output.writeText(configurations.getByName("releaseRuntimeClasspath")
            .resolvedConfiguration.resolvedArtifacts.sortedBy { it.moduleVersion.id.toString() }
            .joinToString("\n") { "${it.moduleVersion.id}\t${it.file.absolutePath}" } + "\n")
    }
}
