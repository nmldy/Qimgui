#include "window_session.h"

#include "aimgui_log.h"
#include "platform/ANativeWindowCreator.h"

namespace aimgui {

bool WindowSession::Build(int side, bool permeate_record) {
    AILOGI("enter side=%d permeate=%d", side, (int)permeate_record);
    m_Window = android::ANativeWindowCreator::Create("AImGui", side, side,
                                                     permeate_record);
    AILOGI("ANativeWindow = %p", m_Window);
    if (!m_Window) {
        AILOGE("ANativeWindowCreator::Create returned null");
        return false;
    }

    AILOGI("calling MakeRenderer ...");
    m_Renderer = MakeRenderer(m_Window, side, side, Backend::Auto);
    AILOGI("m_Renderer = %p", m_Renderer.get());

    if (!m_Renderer) {
        AILOGE("MakeRenderer returned null; destroying window");
        android::ANativeWindowCreator::Destroy(m_Window);
        m_Window = nullptr;
        return false;
    }
    AILOGI("Build complete renderer=%s", m_Renderer->Name());
    return true;
}

void WindowSession::Destroy() {
    AILOGI("enter renderer=%p window=%p", m_Renderer.get(), m_Window);
    if (m_Renderer) { m_Renderer->Shutdown(); m_Renderer.reset(); }
    if (m_Window)   { android::ANativeWindowCreator::Destroy(m_Window); m_Window = nullptr; }
    AILOGI("done");
}

} // namespace aimgui
