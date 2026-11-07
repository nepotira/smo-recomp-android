plugins {
    id("com.android.application") version "8.7.2" apply false
}

tasks.register("clean", Delete::class) {
    delete(rootProject.layout.buildDirectory)
}
