import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// M1 spike: draws 100k GeoPackage points with MapLibre's CustomGeometrySource
// and measures loading, frame rate and the refresh after an edit. Not part of
// the AAR; it goes away when the app rewrite takes over.
android {
    namespace = "org.geocrew.nativelib.mapspike"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.geocrew.nativelib.mapspike"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
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
    // The default android-sdk renders with Vulkan, which crashed on the emulator (null
    // pointer in the render thread while animating the camera); OpenGL ES works there.
    // Build with -PmapBackend=vulkan to try Vulkan on a phone.
    val vulkan = providers.gradleProperty("mapBackend").orNull == "vulkan"
    implementation(if (vulkan) "org.maplibre.gl:android-sdk:13.7.1" else "org.maplibre.gl:android-sdk-opengl:13.7.1")
    // RasterActivity answers MapLibre's tile requests with an OkHttp interceptor.
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
}
