Gradle wrapper JAR
==================

The binary `gradle-wrapper.jar` is intentionally NOT committed here (a binary
blob can't be hand-authored in this scaffold). The `gradlew` / `gradlew.bat`
launcher scripts and the `gradle-wrapper.properties` descriptor (which pins the
Gradle version) ARE provided.

Materialize the jar once, either way:

  * Open the `android/` project in Android Studio — it regenerates the wrapper
    jar automatically on first sync, OR
  * From `android/`, with a system Gradle on PATH, run:
        gradle wrapper --gradle-version 8.7

After that, `./gradlew assembleDebug` works without a system Gradle.

See docs/ANDROID.md for the full prerequisites and build commands.
