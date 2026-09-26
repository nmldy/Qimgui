#include "renderer.h"

#include "bloom_gl.h"
#include "aimgui_log.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <cstdint>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

namespace aimgui {

namespace {

const char* EglErrStr(EGLint e) {
    switch (e) {
        case EGL_SUCCESS:             return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:     return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:          return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:           return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:       return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT:         return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG:          return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:         return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE:         return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH:           return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER:       return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP:   return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW:   return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST:        return "EGL_CONTEXT_LOST";
        default:                      return "EGL_???";
    }
}

class GLRenderer final : public IRenderer {
public:
    bool Init(ANativeWindow* window, int width, int height) override {
        AILOGI("begin window=%p size=%dx%d", window, width, height);
        m_Window = window;
        m_Width = width;
        m_Height = height;

        const EGLint cfg_attribs[] = {
            EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_NONE,
        };
        const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };

        m_Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        AILOGI("eglGetDisplay -> %p", m_Display);
        if (m_Display == EGL_NO_DISPLAY) {
            AILOGE("no display");
            return false;
        }

        if (!eglInitialize(m_Display, nullptr, nullptr)) {
            EGLint e = eglGetError();
            AILOGE("eglInitialize failed err=0x%x (%s)", e, EglErrStr(e));
            return false;
        }
        AILOGI("eglInitialize OK");

        EGLConfig cfg;
        EGLint n = 0;
        if (!eglChooseConfig(m_Display, cfg_attribs, &cfg, 1, &n) || n < 1) {
            EGLint e = eglGetError();
            AILOGE("eglChooseConfig failed n=%d err=0x%x (%s)", n, e, EglErrStr(e));
            return false;
        }
        AILOGI("eglChooseConfig OK (n=%d cfg=%p)", n, (void*)cfg);

        EGLint visual = 0;
        eglGetConfigAttrib(m_Display, cfg, EGL_NATIVE_VISUAL_ID, &visual);
        AILOGI("visual id = 0x%x", visual);

        AILOGI("window before setBuffersGeometry: %dx%d format=%d",
               ANativeWindow_getWidth(window),
               ANativeWindow_getHeight(window),
               ANativeWindow_getFormat(window));

        int rc = ANativeWindow_setBuffersGeometry(window, 0, 0, visual);
        AILOGI("ANativeWindow_setBuffersGeometry -> %d", rc);

        m_Context = eglCreateContext(m_Display, cfg, EGL_NO_CONTEXT, ctx_attribs);
        AILOGI("eglCreateContext -> %p err=0x%x", m_Context, eglGetError());

        m_Surface = eglCreateWindowSurface(m_Display, cfg, window, nullptr);
        AILOGI("eglCreateWindowSurface -> %p err=0x%x", m_Surface, eglGetError());

        if (m_Context == EGL_NO_CONTEXT || m_Surface == EGL_NO_SURFACE) {
            AILOGE("context/surface creation failed (ctx=%p surf=%p)",
                   m_Context, m_Surface);
            return false;
        }

        if (!eglMakeCurrent(m_Display, m_Surface, m_Surface, m_Context)) {
            EGLint e = eglGetError();
            AILOGE("eglMakeCurrent failed err=0x%x (%s)", e, EglErrStr(e));
            return false;
        }
        AILOGI("eglMakeCurrent OK");

        eglSwapInterval(m_Display, 1);

        glViewport(0, 0, width, height);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        GLenum glerr = glGetError();
        if (glerr != GL_NO_ERROR) AILOGW("glClear glGetError=0x%x", glerr);

        AILOGI("ImGui_ImplOpenGL3_Init ...");
        if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
            AILOGE("ImGui_ImplOpenGL3_Init returned false");
            return false;
        }
        AILOGI("ImGui_ImplOpenGL3_Init OK");

        AILOGI("BloomGL::Init ...");
        bool bloomOk = m_Bloom.Init(width, height);
        AILOGI("BloomGL::Init -> %d (Ready=%d)",
               (int)bloomOk, (int)m_Bloom.Ready());

        AILOGI("GL Init complete");
        return true;
    }

    void NewFrame() override {
        ImGui_ImplOpenGL3_NewFrame();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)m_Width, (float)m_Height);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }

    void EndFrame() override {
        ImGui::Render();
        if (m_Bloom.Ready()) {
            m_Bloom.BeginScene();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            m_Bloom.EndSceneAndComposite();
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, m_Width, m_Height);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
        if (!eglSwapBuffers(m_Display, m_Surface)) {
            // Log once to avoid spamming logcat on every frame.
            static bool logged = false;
            if (!logged) {
                logged = true;
                EGLint e = eglGetError();
                AILOGE("eglSwapBuffers failed err=0x%x (%s)", e, EglErrStr(e));
            }
        }
    }

    void Shutdown() override {
        AILOGI("enter");
        m_Bloom.Shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        if (m_Display != EGL_NO_DISPLAY) {
            eglMakeCurrent(m_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_Context != EGL_NO_CONTEXT) eglDestroyContext(m_Display, m_Context);
            if (m_Surface != EGL_NO_SURFACE) eglDestroySurface(m_Display, m_Surface);
            eglTerminate(m_Display);
        }
        m_Display = EGL_NO_DISPLAY;
        m_Surface = EGL_NO_SURFACE;
        m_Context = EGL_NO_CONTEXT;
        AILOGI("done");
    }

    const char* Name() const override { return "OpenGL ES 3"; }

    void SetBloomIntensity(float i) override { m_Bloom.SetIntensity(i); }

    unsigned long long GetSceneSnapshotID() override {
        return (unsigned long long)(uintptr_t)m_Bloom.GetSnapshotTex();
    }

    void SetSnapshotFrozen(bool frozen) override { m_Bloom.SetSnapshotFrozen(frozen); }

private:
    ANativeWindow* m_Window = nullptr;
    EGLDisplay m_Display = EGL_NO_DISPLAY;
    EGLSurface m_Surface = EGL_NO_SURFACE;
    EGLContext m_Context = EGL_NO_CONTEXT;
    int m_Width = 0;
    int m_Height = 0;
    BloomGL m_Bloom;
};

} // namespace

std::unique_ptr<IRenderer> MakeGLRenderer() {
    return std::unique_ptr<IRenderer>(new GLRenderer());
}

} // namespace aimgui
