# SMO Recomp Proguard Rules
-keep class com.nepo.smorecomp.** { *; }
-keepclassmembers class * {
    native <methods>;
}
