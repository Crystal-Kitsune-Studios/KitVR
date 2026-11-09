plugins {
    id("com.android.application")
}

val releaseKeystorePath = providers.environmentVariable("ANDROID_KEYSTORE_PATH").orElse("release.keystore").get()
val releaseKeystorePassword = providers.environmentVariable("ANDROID_KEYSTORE_PASSWORD").orElse("").get()
val releaseKeyAlias = providers.environmentVariable("ANDROID_KEY_ALIAS").orElse("").get()
val releaseKeyPassword = providers.environmentVariable("ANDROID_KEY_PASSWORD").orElse("").get()

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
    implementation("org.khronos.openxr:openxr_loader_for_android:1.0.34")
}
