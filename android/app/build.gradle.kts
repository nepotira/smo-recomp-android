plugins {
    id("com.android.application")
}

android {
    namespace = "com.mobileodyssey.game"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.nepo.smorecomp"
        minSdk = 30
        targetSdk = 35
        versionCode = 11
        versionName = "0.2.0"

        ndk {
            abiFilters.add("arm64-v8a")
        }
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
            pickFirsts.addAll(listOf(
                "**/libsmo.so",
                "**/libfile_redirect_hook.so",
                "**/libgsl_alloc_hook.so",
                "**/libhook_impl.so",
                "**/libmain_hook.so"
            ))
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("debug")
        }
        debug {
            isDebuggable = true
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
}
