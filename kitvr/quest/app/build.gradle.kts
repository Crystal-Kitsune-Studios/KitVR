plugins {
    id("com.android.application")
}

android {
    namespace = "com.crystalkitsune.kitvr"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.crystalkitsune.kitvr"
        minSdk = 29
        targetSdk = 32
        versionCode = 1
        versionName = "0.1.0"

        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_STL=c++_shared") }
        }
    }

    buildFeatures { prefab = true }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }
}

dependencies {
    // 1.0.34 matches the OpenXR 1.0 API version that Quest runtimes accept without surprises.
    implementation("org.khronos.openxr:openxr_loader_for_android:1.0.34")
}
