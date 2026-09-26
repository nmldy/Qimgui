#include "renderer.h"

#include "aimgui_log.h"

namespace aimgui {

std::unique_ptr<IRenderer> MakeRenderer(ANativeWindow* window,
                                        int width, int height,
                                        Backend preferred) {
    AILOGI("enter window=%p %dx%d preferred=%d",
           window, width, height, (int)preferred);

    auto tryInit = [&](std::unique_ptr<IRenderer> r,
                       const char* name) -> std::unique_ptr<IRenderer> {
        if (!r) {
            AILOGE("factory returned null for %s", name);
            return nullptr;
        }
        AILOGI("calling %s->Init ...", name);
        if (r->Init(window, width, height)) {
            AILOGI("%s Init OK (Name=%s)", name, r->Name());
            return r;
        }
        AILOGE("%s Init FAILED; tearing down", name);
        r->Shutdown();
        return nullptr;
    };

    if (preferred == Backend::OpenGL)
        return tryInit(MakeGLRenderer(), "OpenGL");
    if (preferred == Backend::Vulkan)
        return tryInit(MakeVKRenderer(), "Vulkan");

    if (auto vk = tryInit(MakeVKRenderer(), "Vulkan")) return vk;
    AILOGW("Vulkan init failed, falling back to OpenGL ES 3");
    return tryInit(MakeGLRenderer(), "OpenGL");
}

} // namespace aimgui
