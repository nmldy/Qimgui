// AImGui: a minimal Dear ImGui Android ARM64 ELF.
#include "core/font.h"
#include "core/frame_pacer.h"
#include "core/keyboard_input.h"
#include "core/window_session.h"
#include "core/aimgui_log.h"       // ← 新增：统一日志
#include "core/crash_handler.h"    // ← 新增：崩溃捕获
#include "imgui.h"
#include "platform/ANativeWindowCreator.h"
#include "platform/TouchHelperA.h"
#include "ui/ui.h"

#include <chrono>

int main() {
    using namespace android;
    using clock = std::chrono::steady_clock;

    // 装崩溃处理器要放在最前面：后面任何一步（包括字体加载、窗口创建、
    // Vulkan/GL 初始化）崩了，报告都会落到文件里。
    aimgui::crash::Install();

    AILOGI("=== AImGui starting ===");
    AILOGI("crash report path: %s", aimgui::crash::ReportPath());

    auto info = ANativeWindowCreator::GetDisplayInfo();
    const int W = info.width > info.height ? info.width : info.height;
    const int H = info.width > info.height ? info.height : info.width;
    AILOGI("display %ux%u orientation=%u",
           (unsigned)info.width, (unsigned)info.height, (unsigned)info.orientation);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    AILOGI("ImGui context created version=%s", ImGui::GetVersion());

    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // ImGui 1.92's stb font loader raises a *recoverable* IM_ASSERT_USER_ERROR
    // when a system font fails to parse. By default that aborts the process
    // (SIGABRT) on first text render. Disable the assert so a bad/unsupported
    // system font degrades gracefully (logged, skipped) instead of crashing.
    io.ConfigErrorRecoveryEnableAssert = false;
    ImGui::StyleColorsDark();

    AILOGI("loading system CJK font ...");
    aimgui::LoadDefaultAndSystemCJKFont(25.0f);
    AILOGI("font loaded");

    aimgui::UiState st;
    st.display_w = info.width; st.display_h = info.height;

    aimgui::WindowSession ws;
    AILOGI("WindowSession::Build ...");
    if (!ws.Build(W, st.permeate_record)) {
        AILOGE("WindowSession::Build failed; exiting");
        ImGui::DestroyContext();
        return 1;
    }
    st.renderer_name = ws.renderer()->Name();
    AILOGI("renderer selected: %s", st.renderer_name);

    AILOGI("Touch::Init ...");
    Touch::Init({(float)W, (float)H}, false);
    Touch::setOrientation((int)info.orientation);
    AILOGI("Touch::Init done");

    AILOGI("kbd_input::Init ...");
    aimgui::kbd_input::Init();
    AILOGI("kbd_input::Init done");

    aimgui::FramePacer pacer;
    auto last = clock::now();
    uint32_t orient = info.orientation;
    bool running = true;

    AILOGI("=== entering main loop ===");
    while (running) {
        auto now = clock::now();
        io.DeltaTime = std::max(1e-6f, std::chrono::duration<float>(now - last).count());
        last = now;
        pacer.SetTargetFps(st.target_fps);
        info = ANativeWindowCreator::GetDisplayInfo();
        st.display_w = info.width; st.display_h = info.height;
        if (info.orientation != orient) {
            orient = info.orientation;
            Touch::setOrientation((int)orient);
            AILOGI("orientation changed to %u", orient);
        }
        if (aimgui::kbd_input::ConsumeVolumePresses() > 0) st.collapsed = !st.collapsed;
        if (!st.permeate_record) ANativeWindowCreator::ProcessMirrorDisplay();
        aimgui::kbd_input::Flush();

        ws.renderer()->NewFrame();
        st.scene_snapshot_id = ws.renderer()->GetSceneSnapshotID();
        ImGui::NewFrame();
        aimgui::DrawUi(&st, &running);
        ws.renderer()->SetBloomIntensity(st.bloom_intensity);
        ws.renderer()->SetSnapshotFrozen(st.exit_anim_active);
        ws.renderer()->EndFrame();
        pacer.Wait();

        if (st.request_permeate_toggle) {
            AILOGI("permeate toggle requested");
            st.request_permeate_toggle = false;
            st.permeate_record = !st.permeate_record;
            ws.Destroy();
            if (!ws.Build(W, st.permeate_record)) { running = false; break; }
            st.renderer_name = ws.renderer()->Name();
            AILOGI("rebuilt with renderer=%s", st.renderer_name);
        }
    }
    AILOGI("=== exiting main loop ===");

    aimgui::kbd_input::Shutdown();
    ws.Destroy();
    ImGui::DestroyContext();
    AILOGI("=== AImGui shutdown complete ===");
    return 0;
}
