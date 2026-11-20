// Asura's Wrath Recompiled for Android: a thin Kotlin activity (SDL's
// SDLActivity) around the same native code as the desktop build. The native
// part is the repo's own CMakeLists.txt, built by the NDK for arm64.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val repoRoot = rootProject.projectDir.parentFile
val sdlAndroid = File(repoRoot, "thirdparty/rexglue/thirdparty/sdl3/android-project/app/src/main")

android {
    namespace = "org.asurawrath.recompiled"
    compileSdk = 35
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "org.asurawrath.recompiled"
        // Android 10: arm64 devices there ship Vulkan 1.1, and the SDK's
        // shared memory (ASharedMemory) is there.
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.4.2"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // The SDK names its libraries by build type (librexruntimerd.so
                    // for RelWithDebInfo, AGP's choice for release): plain names
                    // for the activity's loadLibrary.
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DREXGLUE_ENABLE_TRACY=OFF",
                    "-DASURA_BUILD_SHADERGEN=OFF",
                )
                // The game library and what it loads: the runtime and the GPU plugins.
                targets += listOf("asura_wrath", "rexruntime", "rexgpu-mantra", "rexgpu-xenos")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = File(repoRoot, "CMakeLists.txt")
            version = "3.31.6"
            // Next to the desktop builds (out/build/<preset>).
            buildStagingDirectory = File(repoRoot, "out/build/android-gradle")
        }
    }

    sourceSets["main"].apply {
        // SDL's Java side (SDLActivity and friends), straight from the SDK's SDL.
        java.srcDirs(File(sdlAndroid, "java"), "src/main/kotlin")
        // The launcher's own art (assets/ui), read through the APK's assets.
        assets.srcDirs(File(repoRoot, "assets"))
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Signed with the debug key until there is a release key.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    packaging {
        // The native libraries load from the APK's lib/ folder as files
        // (dlopen of the GPU plugin by path).
        jniLibs.useLegacyPackaging = true
    }
    lint { abortOnError = false }
}
