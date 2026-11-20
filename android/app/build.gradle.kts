// Asura's Wrath Recompiled for Android: a thin Kotlin activity (SDL's
// SDLActivity) around the same native code as the desktop build. The native
// part is the repo's own CMakeLists.txt, built by the NDK for each ABI.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val repoRoot = rootProject.projectDir.parentFile
val sdlAndroid = File(repoRoot, "thirdparty/rexglue/thirdparty/sdl3/android-project/app/src/main")
// 64-bit only (the guest's address space doesn't fit 32 bits): arm64 for
// phones, x86_64 for emulators and x86 Chromebooks. -Pasura.abis=arm64-v8a
// builds just one.
val abis = (findProperty("asura.abis") as String? ?: "arm64-v8a,x86_64").split(",").map { it.trim() }
// The shader cache versions the game accepts (the launcher checks an
// imported cache against them), from mantra's own header.
val shaderCacheFormat = File(repoRoot, "src/mantra_renderer/shader_cache_format.h").readText()
fun shaderCacheConstant(name: String): String =
    Regex("""constexpr uint32_t $name = (\d+);""").find(shaderCacheFormat)?.groupValues?.get(1)
        ?: error("$name not found in shader_cache_format.h")

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
        buildConfigField("int", "SHADER_CACHE_VERSION", shaderCacheConstant("kVersion"))
        buildConfigField("int", "SHADER_CACHE_MIN_VERSION", shaderCacheConstant("kMinVersion"))
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // The SDK names its libraries by build type (librexruntimerd.so
                    // for RelWithDebInfo, AGP's choice for release): plain names
                    // for the activity's loadLibrary.
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DREXGLUE_ENABLE_TRACY=OFF",
                    "-DASURA_DXC_ANDROID_DIR=${File(repoRoot, "out/dxc/jniLibs")}",
                )
                // The game library and what it loads: the runtime and the GPU
                // plugins; and mantra_shadergen (libmantra_shadergen.so, a
                // program: the launcher's shader build).
                targets += listOf("asura_wrath", "rexruntime", "rexgpu-mantra", "rexgpu-xenos",
                                  "mantra_shadergen")
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
        // DXC, cross-built by tools/build_dxc_android.sh (mantra_shadergen's).
        jniLibs.srcDirs(File(repoRoot, "out/dxc/jniLibs"))
    }

    // An APK per ABI, plus a universal one with all of them.
    splits {
        abi {
            isEnable = true
            reset()
            include(*abis.toTypedArray())
            isUniversalApk = abis.size > 1
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Signed with the debug key until there is a release key.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    buildFeatures { buildConfig = true }

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

dependencies {
    // The launcher's screen (LauncherActivity): Material components.
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
}
