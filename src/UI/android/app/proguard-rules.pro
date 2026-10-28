# SDL registers/calls these Java methods from libSDL2 via JNI and reflection.
-keep class org.libsdl.app.** { *; }
# These names form the C++ JNI entry point contract.
-keep class com.bgiplayer.bgi_ui_demo.GameActivity { *; }
