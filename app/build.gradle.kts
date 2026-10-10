plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val releaseSigningEnv = listOf("AIRSCREEN_KEYSTORE", "AIRSCREEN_KEYSTORE_PASSWORD", "AIRSCREEN_KEY_ALIAS", "AIRSCREEN_KEY_PASSWORD")
    .associateWith { providers.environmentVariable(it).orNull?.takeIf(String::isNotEmpty) }
val releaseSigned = releaseSigningEnv.values.all { it != null }
if (!releaseSigned && releaseSigningEnv.values.any { it != null }) {
    throw GradleException("release signing needs all of ${releaseSigningEnv.keys}, missing ${releaseSigningEnv.filterValues { it == null }.keys}")
}

/** `-PversionName=1.2.0` sets versionName, and versionCode becomes major * 1000000 + minor * 1000 + patch. */
val versionNameProperty = providers.gradleProperty("versionName").orNull
val versionCodeFromName = versionNameProperty?.let { name ->
    val parts = Regex("""^(\d+)\.(\d+)(?:\.(\d+))?""").find(name)?.groupValues?.drop(1)?.map { it.toIntOrNull() ?: 0 }
        ?: throw GradleException("versionName '$name' does not start with MAJOR.MINOR or MAJOR.MINOR.PATCH")
    val (major, minor, patch) = parts
    if (major > 2099 || minor > 999 || patch > 999 || major + minor + patch == 0) {
        throw GradleException("versionName '$name' is outside 0.0.1 to 2099.999.999")
    }
    major * 1_000_000 + minor * 1_000 + patch
}

android {
    namespace = "dev.airscreen"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "dev.airscreen"
        minSdk = 30
        targetSdk = 35
        versionCode = versionCodeFromName ?: 1
        versionName = versionNameProperty ?: "0.1"
        ndk { abiFilters += listOf("armeabi-v7a", "arm64-v8a") }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    signingConfigs {
        if (releaseSigned) {
            create("release") {
                storeFile = file(releaseSigningEnv.getValue("AIRSCREEN_KEYSTORE")!!)
                storePassword = releaseSigningEnv.getValue("AIRSCREEN_KEYSTORE_PASSWORD")
                keyAlias = releaseSigningEnv.getValue("AIRSCREEN_KEY_ALIAS")
                keyPassword = releaseSigningEnv.getValue("AIRSCREEN_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (releaseSigned) "release" else "debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

gradle.taskGraph.whenReady {
    if (!releaseSigned && allTasks.any { it.path == ":app:packageRelease" }) {
        logger.warn("WARNING: AIRSCREEN_KEYSTORE is not set, so the release APK is signed with the debug key. Do not publish it.")
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
