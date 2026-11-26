import org.gradle.api.initialization.resolve.RepositoriesMode

// All repositories are mirrored through Aliyun first: the mirrors are much
// faster than google()/mavenCentral() from mainland China, and they are the
// only reliable route for the machines this project is built on. The upstream
// repositories stay as a fallback.
pluginManagement {
    repositories {
        maven("https://maven.aliyun.com/repository/google")
        maven("https://maven.aliyun.com/repository/central")
        maven("https://maven.aliyun.com/repository/gradle-plugin")
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven("https://maven.aliyun.com/repository/google")
        maven("https://maven.aliyun.com/repository/central")
        maven("https://maven.aliyun.com/repository/gradle-plugin")
        google()
        mavenCentral()
    }
}

rootProject.name = "molink-android"
include(":app")
