plugins { id("com.android.application") version "9.5.0" apply false }

// Keep root reports off the constrained source drive as well as app/native outputs.
providers.environmentVariable("POCKETVR_BUILD_ROOT").orNull?.let {
    layout.buildDirectory.set(file("$it/android/root"))
}
