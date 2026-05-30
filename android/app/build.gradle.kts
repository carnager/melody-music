import java.io.File
import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

val releaseSigningProperties = Properties()
val releaseSigningPropertiesFile = file(
    System.getenv("MELODY_ANDROID_SIGNING_PROPERTIES")
        ?: "${System.getProperty("user.home")}/.local/android/release-keys/melody.properties"
)
if (releaseSigningPropertiesFile.isFile) {
    releaseSigningPropertiesFile.inputStream().use(releaseSigningProperties::load)
}

fun signingValue(name: String): String? =
    providers.gradleProperty(name).orNull
        ?: System.getenv(name)
        ?: releaseSigningProperties.getProperty(name)

fun signingFile(path: String): File =
    if (path.startsWith("~/")) {
        File(System.getProperty("user.home"), path.removePrefix("~/"))
    } else {
        file(path)
    }

android {
    namespace = "com.melody.app"
    compileSdk = 36
    buildToolsVersion = "37.0.0"

    defaultConfig {
        applicationId = "com.melody.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 12
        versionName = "1.1.1"
    }

    signingConfigs {
        create("release") {
            val storePath = signingValue("MELODY_ANDROID_STORE_FILE")
            if (storePath != null) {
                storeFile = signingFile(storePath)
                storePassword = signingValue("MELODY_ANDROID_STORE_PASSWORD")
                keyAlias = signingValue("MELODY_ANDROID_KEY_ALIAS")
                keyPassword = signingValue("MELODY_ANDROID_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            if (signingValue("MELODY_ANDROID_STORE_FILE") != null) {
                signingConfig = signingConfigs.getByName("release")
            }
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    lint {
        checkReleaseBuilds = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }
}

dependencies {
    // Compose BOM
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.navigation:navigation-compose:2.8.5")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")

    // ExoPlayer for audio playback (HLS, seeking, all formats)
    implementation("androidx.media3:media3-exoplayer:1.6.1")
    implementation("androidx.media3:media3-session:1.6.1")

    // Networking
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    implementation("org.json:json:20231013")

    // Image loading
    implementation("io.coil-kt.coil3:coil-compose:3.1.0")
    implementation("io.coil-kt.coil3:coil-network-okhttp:3.1.0")

    // Preferences
    implementation("androidx.datastore:datastore-preferences:1.1.1")

    implementation("androidx.compose.material:material-icons-extended")

    debugImplementation("androidx.compose.ui:ui-tooling")
}
