plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "pt.samuel.detetordrones"
    compileSdk = 35
    defaultConfig {
        applicationId = "pt.samuel.detetordrones"
        minSdk = 31
        targetSdk = 35
        versionCode = 3
        versionName = "1.2"
    }
    // Chave de assinatura fixa, vinda dos Secrets do GitHub (KEYSTORE_B64 / KEYSTORE_PASSWORD).
    // Sem ela, cada compilação usa uma chave nova e as atualizações não instalam por cima.
    val ksPath: String? = System.getenv("KEYSTORE_PATH")
    signingConfigs {
        if (ksPath != null) create("fixa") {
            storeFile = file(ksPath)
            storePassword = System.getenv("KEYSTORE_PASSWORD")
            keyAlias = "detetor"
            keyPassword = System.getenv("KEYSTORE_PASSWORD")
        }
    }
    buildTypes {
        getByName("debug") {
            if (ksPath != null) signingConfig = signingConfigs.getByName("fixa")
        }
    }
    buildFeatures { compose = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation(platform("androidx.compose:compose-bom:2024.10.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
}
