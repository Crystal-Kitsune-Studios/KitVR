plugins {
    id("com.android.application")
}

val releaseKeystorePath = providers.environmentVariable("ANDROID_KEYSTORE_PATH").orElse("release.keystore").get()
val releaseKeystorePassword = providers.environmentVariable("ANDROID_KEYSTORE_PASSWORD").orElse("").get()
val releaseKeyAlias = providers.environmentVariable("ANDROID_KEY_ALIAS").orElse("").get()
val releaseKeyPassword = providers.environmentVariable("ANDROID_KEY_PASSWORD").orElse("").get()
val gitRef = providers.environmentVariable("GITHUB_REF_NAME").orElse("").get()
val appVersion = if (gitRef.matches(Regex("v?\\d+\\.\\d+\\.\\d+"))) {
    gitRef.removePrefix("v")
} else {
    "0.1.1"
}
val versionParts = appVersion.split(".").map { it.toInt() }
require(versionParts.size == 3) { "App version must use major.minor.patch: $appVersion" }

android {
    namespace = "com.crystalkitsune.kitvr"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.crystalkitsune.kitvr"
        minSdk = 29
        targetSdk = 33
        versionCode = versionParts[0] * 10000 + versionParts[1] * 100 + versionParts[2]
        versionName = appVersion

        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake { arguments += listOf("-DANDROID_STL=c++_shared") }
        }
    }

    signingConfigs {
        create("release") {
            storeFile = file(releaseKeystorePath)
            storePassword = releaseKeystorePassword
            keyAlias = releaseKeyAlias
            keyPassword = releaseKeyPassword
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
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }
}

dependencies {
    implementation("androidx.core:core:1.13.1")
    implementation("org.khronos.openxr:openxr_loader_for_android:1.0.34")
}
