// DualScreenHUD: redirects the core in-game HUD (hearts, magic, rupees, minimap, action buttons -
// i.e. everything drawn via Interface_Draw) onto the secondary physical screen of dual-screen Android
// devices (e.g. Surface Duo, LG dual-screen cases), while leaving single-screen devices and 3D world
// rendering completely untouched.
//
// How it works:
//   1. The Android Java layer (MainActivity + DualScreenManager/DualScreenHudPresentation) detects a
//      secondary Display with FLAG_PRESENTATION, hosts a Presentation/SurfaceView on it, and forwards
//      Surface lifecycle events to native code via the JNI functions at the bottom of this file.
//   2. Interface_Draw's OVERLAY_DISP output is redirected (see Play_DrawOverlayElements in z_play.c)
//      into an offscreen framebuffer (see framebuffer_effects.c for the established pattern) instead of
//      the main framebuffer.
//   3. Once per frame, after the main frame has been rendered (see Graph_ProcessGfxCommands in
//      OTRGlobals.cpp), DualScreenHUD_PresentFrame() blits that offscreen framebuffer's texture onto the
//      secondary display's Surface using a small shared-context EGL surface.
#include "DualScreenHUD.h"

#include <libultraship/libultraship.h>
#include <graphic/Fast3D/gfx_pc.h>
#include <graphic/Fast3D/gfx_rendering_api.h>
#include "public/bridge/consolevariablebridge.h"

#include "soh/cvar_prefixes.h"

extern "C" {
int gfx_create_framebuffer(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                            uint8_t resize);
}

#define DUAL_SCREEN_HUD_WIDTH 320
#define DUAL_SCREEN_HUD_HEIGHT 240

static int32_t sHudFrameBufferId = -1;

int32_t DualScreenHUD_GetFrameBufferId(void) {
    if (sHudFrameBufferId == -1) {
        sHudFrameBufferId = gfx_create_framebuffer(DUAL_SCREEN_HUD_WIDTH, DUAL_SCREEN_HUD_HEIGHT,
                                                    DUAL_SCREEN_HUD_WIDTH, DUAL_SCREEN_HUD_HEIGHT, true);
    }
    return sHudFrameBufferId;
}

#if defined(__ANDROID__)

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <mutex>

namespace {

struct DualScreenHudState {
    std::mutex mutex;

    // Set by the JNI callbacks (Android UI/Presentation thread), consumed on the render thread the next
    // time DualScreenHUD_PresentFrame() runs.
    ANativeWindow* pendingWindow = nullptr;
    bool pendingDestroy = false;

    // Only ever touched on the render thread.
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    ANativeWindow* window = nullptr;

    GLuint shaderProgram = 0;
    GLuint vbo = 0;
    bool glResourcesReady = false;
};

DualScreenHudState sState;

const char* kVertexShaderSrc = "attribute vec2 aPosition;\n"
                               "attribute vec2 aTexCoord;\n"
                               "varying vec2 vTexCoord;\n"
                               "void main() {\n"
                               "    vTexCoord = aTexCoord;\n"
                               "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
                               "}\n";

const char* kFragmentShaderSrc = "precision mediump float;\n"
                                 "varying vec2 vTexCoord;\n"
                                 "uniform sampler2D uTexture;\n"
                                 "void main() {\n"
                                 "    gl_FragColor = texture2D(uTexture, vTexCoord);\n"
                                 "}\n";

GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    return shader;
}

// Must only be called while sState.surface is current.
void EnsureGlResources() {
    if (sState.glResourcesReady) {
        return;
    }

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, kVertexShaderSrc);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShaderSrc);

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aTexCoord");
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    // Fullscreen quad (as a triangle strip). The HUD framebuffer's texture is sampled with (0,0) at the
    // top-left, matching how it is written to by the main renderer.
    static const float verts[] = {
        // x,     y,     u,    v
        -1.0f, -1.0f, 0.0f, 1.0f, //
        1.0f,  -1.0f, 1.0f, 1.0f, //
        -1.0f, 1.0f,  0.0f, 0.0f, //
        1.0f,  1.0f,  1.0f, 0.0f, //
    };
    glGenBuffers(1, &sState.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, sState.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    sState.shaderProgram = program;
    sState.glResourcesReady = true;
}

// Must be called with sState.mutex held, on the render thread.
void DestroySecondarySurfaceLocked() {
    if (sState.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(sState.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (sState.surface != EGL_NO_SURFACE) {
            eglDestroySurface(sState.display, sState.surface);
        }
        if (sState.context != EGL_NO_CONTEXT) {
            eglDestroyContext(sState.display, sState.context);
        }
    }
    if (sState.window != nullptr) {
        ANativeWindow_release(sState.window);
    }
    sState.display = EGL_NO_DISPLAY;
    sState.context = EGL_NO_CONTEXT;
    sState.surface = EGL_NO_SURFACE;
    sState.window = nullptr;
    sState.glResourcesReady = false;
}

// Must be called with sState.mutex held, on the render thread, while the main render context is current.
// Takes ownership of `window` on success; caller must release it on failure.
bool CreateSecondarySurfaceLocked(ANativeWindow* window) {
    EGLDisplay mainDisplay = eglGetCurrentDisplay();
    EGLContext mainContext = eglGetCurrentContext();
    if (mainDisplay == EGL_NO_DISPLAY || mainContext == EGL_NO_CONTEXT) {
        return false;
    }

    const EGLint configAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                                     EGL_RED_SIZE,        8,
                                     EGL_GREEN_SIZE,      8,
                                     EGL_BLUE_SIZE,       8,
                                     EGL_ALPHA_SIZE,      8,
                                     EGL_NONE };
    EGLConfig config;
    EGLint numConfigs = 0;
    if (eglChooseConfig(mainDisplay, configAttribs, &config, 1, &numConfigs) != EGL_TRUE || numConfigs == 0) {
        return false;
    }

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    // Shares texture/buffer objects with the main context so the HUD framebuffer's texture id is valid here.
    EGLContext context = eglCreateContext(mainDisplay, config, mainContext, ctxAttribs);
    if (context == EGL_NO_CONTEXT) {
        return false;
    }

    EGLSurface surface = eglCreateWindowSurface(mainDisplay, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        eglDestroyContext(mainDisplay, context);
        return false;
    }

    sState.display = mainDisplay;
    sState.context = context;
    sState.surface = surface;
    sState.window = window;
    return true;
}

} // namespace

uint8_t DualScreenHUD_IsActive(void) {
    std::lock_guard<std::mutex> lock(sState.mutex);
    bool hasSecondaryDisplay = sState.window != nullptr || sState.pendingWindow != nullptr;
    return hasSecondaryDisplay && CVarGetInteger(CVAR_ENHANCEMENT("DualScreenHUD"), 1) != 0;
}

void DualScreenHUD_PresentFrame(void) {
    std::lock_guard<std::mutex> lock(sState.mutex);

    if (sState.pendingDestroy) {
        DestroySecondarySurfaceLocked();
        sState.pendingDestroy = false;
    }

    if (sState.pendingWindow != nullptr) {
        ANativeWindow* newWindow = sState.pendingWindow;
        sState.pendingWindow = nullptr;
        DestroySecondarySurfaceLocked();
        if (!CreateSecondarySurfaceLocked(newWindow)) {
            ANativeWindow_release(newWindow);
        }
    }

    if (sState.surface == EGL_NO_SURFACE || CVarGetInteger(CVAR_ENHANCEMENT("DualScreenHUD"), 1) == 0) {
        return;
    }

    int32_t fbId = DualScreenHUD_GetFrameBufferId();
    if (fbId == -1) {
        return;
    }

    GfxRenderingAPI* rapi = gfx_get_current_rendering_api();
    if (rapi == nullptr) {
        return;
    }
    GLuint hudTexture = (GLuint)(uintptr_t)rapi->get_framebuffer_texture_id(fbId);

    // Save the primary window's current EGL bindings so we can restore them afterwards - this call
    // must not disturb the main render loop.
    EGLDisplay prevDisplay = eglGetCurrentDisplay();
    EGLSurface prevDraw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prevRead = eglGetCurrentSurface(EGL_READ);
    EGLContext prevContext = eglGetCurrentContext();

    if (eglMakeCurrent(sState.display, sState.surface, sState.surface, sState.context) != EGL_TRUE) {
        return;
    }

    EnsureGlResources();

    EGLint width = 0;
    EGLint height = 0;
    eglQuerySurface(sState.display, sState.surface, EGL_WIDTH, &width);
    eglQuerySurface(sState.display, sState.surface, EGL_HEIGHT, &height);

    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(sState.shaderProgram);
    glBindBuffer(GL_ARRAY_BUFFER, sState.vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hudTexture);
    glUniform1i(glGetUniformLocation(sState.shaderProgram, "uTexture"), 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    eglSwapBuffers(sState.display, sState.surface);

    // Restore the primary window's context/surfaces so subsequent game rendering is unaffected.
    eglMakeCurrent(prevDisplay, prevDraw, prevRead, prevContext);
}

extern "C" JNIEXPORT void JNICALL Java_com_dishii_soh_MainActivity_nativeSecondaryDisplaySurfaceChanged(
    JNIEnv* env, jobject thiz, jobject surface, jint width, jint height) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(sState.mutex);
    if (sState.pendingWindow != nullptr) {
        ANativeWindow_release(sState.pendingWindow);
    }
    sState.pendingWindow = window;
    sState.pendingDestroy = false;
}

extern "C" JNIEXPORT void JNICALL
Java_com_dishii_soh_MainActivity_nativeSecondaryDisplaySurfaceDestroyed(JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> lock(sState.mutex);
    if (sState.pendingWindow != nullptr) {
        ANativeWindow_release(sState.pendingWindow);
        sState.pendingWindow = nullptr;
    }
    sState.pendingDestroy = true;
}

#else // !defined(__ANDROID__)

uint8_t DualScreenHUD_IsActive(void) {
    return 0;
}

void DualScreenHUD_PresentFrame(void) {
}

#endif
