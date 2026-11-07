// Digimon Battle Chronicle for Android: SDL2 frontend over the recompiled game, the HAL,
// the gcn-recomp renderer (OpenGL ES 3.2) and the shared desktop launcher/settings.
// The APK contains code translated from your disc: build it for your own device only.
plugins {
    id("com.android.application")
}

android {
    namespace = "jp.digimonbc.port"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "jp.digimonbc.port"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildTypes {
        // The generated game code is unplayably slow at -O0, so the release variant is
        // the one to install; it is signed with the local debug key.
        release {
            isMinifyEnabled = false
            // Debuggable only so `adb shell run-as` can read logs/saves; native code stays -O2.
            isDebuggable = true
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    // Native Material 3 launcher. The game itself is SDL/ImGui; this is only the
    // Android startup screen, so the desktop launcher stays exactly as it is.
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity:1.9.3")
    implementation("androidx.core:core:1.15.0")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("com.google.android.material:material:1.12.0")
}
