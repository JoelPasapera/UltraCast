plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.ultracast.receiver"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.ultracast.receiver"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17 -O3"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.18.0")
    implementation("com.google.android.material:material:1.13.0")
}