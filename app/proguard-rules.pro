# Add project specific ProGuard rules here.
# You can control the shrinking, obfuscation and optimization of your build.

# Native engine JNI methods
-keep class com.impulser.engine.** {
    native <methods>;
}

-keep class com.impulser.capture.** {
    native <methods>;
}

# Kotlin reflection
-keep class kotlin.Metadata { *; }
-keep class kotlin.reflect.** { *; }

# AndroidX
-keep class androidx.** { *; }