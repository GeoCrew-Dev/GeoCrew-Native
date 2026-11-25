import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The smallest app that ships the native libraries: its release APK size is
// what GDAL, PROJ and GEOS add to the real app.
android {
    namespace = "org.geocrew.nativelib.sample"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.geocrew.nativelib.sample"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }
    // One APK per ABI, as releases will ship; the size budget is for the arm64 one.
    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "x86_64")
            isUniversalApk = false
        }
    }
    // -PlegacyPackaging compresses the native libraries inside the APK (smaller
    // download, but extracted on install) instead of storing them uncompressed.
    packaging {
        jniLibs.useLegacyPackaging = providers.gradleProperty("legacyPackaging").isPresent
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}

dependencies {
    implementation(project(":geocrew-native"))
}
