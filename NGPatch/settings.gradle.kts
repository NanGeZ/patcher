pluginManagement {
    repositories {
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        google()
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        mavenCentral()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.PREFER_SETTINGS)
    repositories {
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        google()
        mavenCentral()
    }
}
rootProject.name = "NGPatch"
include(":app")
