package com.example.threedchess;

import org.libsdl.app.SDLActivity;

/**
 * 3D Chess Android entry Activity.
 *
 * Extends SDL2's {@link org.libsdl.app.SDLActivity}, which sets up the
 * GLSurfaceView + EGL context, marshals touch / key input, and spins up the
 * native thread that calls {@code SDL_main} (our {@code main()} in
 * main_android.cpp). All gameplay/rendering lives in the native libmain.so;
 * this class only declares which shared libraries to load and in what order.
 *
 * NOTE: org.libsdl.app.* is supplied by the vendored SDL2 checkout via
 * app/build.gradle's sourceSets (java.srcDirs → SDL/android-project/.../java),
 * or by the SDL2 prefab AAR. See docs/ANDROID.md.
 */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // Load order matters: SDL2 first, then our engine library. (If you
        // build SDL2 as a static lib instead, drop "SDL2" here.)
        return new String[] {
            "SDL2",
            "main",
        };
    }
}
