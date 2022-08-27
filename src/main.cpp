#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include <mpv/client.h>

#include <switch.h>
#include <deko3d.hpp>

#include "imgui/imgui.h"
#include "implot/implot.h"
#include "imgui_impl_hos/imgui_nx.h"

#include "libmpv.hpp"
#include "render.hpp"
#include "ui/player_gui.hpp"

using namespace std::chrono_literals;

namespace {

constexpr bool USE_FAST_PRESENTATION_PATH = true;

extern "C" void userAppInit() {
    // Keep the main thread above others so that the program stays responsive
    // when doing software decoding
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);

    appletLockExit();

    plInitialize(PlServiceType_User);
    hidInitializeTouchScreen();
    romfsInit();

#ifdef DEBUG
    socketInitializeDefault();
    nxlinkStdio();
#endif
}

extern "C" void userAppExit() {
    appletUnlockExit();

    plExit();
    romfsExit();

#ifdef DEBUG
    socketExit();
#endif
}

} // namespace

int main(int argc, const char **argv) {
    auto file_path = (argc > 1) ? argv[1] : "/Videos/yryr.mkv";

    std::printf("Starting player\n");

    PadState pad;
    HidTouchScreenState touch_state;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    auto lmpv = ampnx::LibmpvController(
#ifdef DEBUG
        [](void*, mpv_event_log_message *msg) {
            std::printf("[%s]: %s", msg->prefix, msg->text);
        }
#endif
    );

    if (lmpv.initialize()) {
        std::printf("Failed to initialize libmpv\n");
        return 1;
    }

    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGui::GetIO().IniFilename = nullptr;

    ImGui::nx::init();

    auto renderer = ampnx::Renderer();
    if (renderer.initialize(lmpv)) {
        std::printf("Failed to initialize renderer\n");
        return 1;
    }

    auto player_ui = ampnx::PlayerGui(renderer, lmpv);

    lmpv.load_file(file_path);

    if (!USE_FAST_PRESENTATION_PATH)
        renderer.switch_presentation_mode(false);

    bool old_ui_visible = false, want_paused_clear = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        auto has_touches = hidGetTouchScreenStates(&touch_state, 1);

        lmpv.process_events();

        if (!player_ui.update_state(pad, touch_state))
            break;

        bool ui_visible = player_ui.is_visible();
        if (USE_FAST_PRESENTATION_PATH &&  ui_visible != old_ui_visible) {
            renderer.switch_presentation_mode(!ui_visible);
            if (player_ui.is_paused())
                want_paused_clear = old_ui_visible == true;
            old_ui_visible = ui_visible;
        }

        if (!USE_FAST_PRESENTATION_PATH || ui_visible) {
            ImGui::nx::newFrame(&pad, has_touches ? &touch_state : nullptr);

            renderer.begin_frame();

            player_ui.render();

            renderer.end_frame();
        } else {
            // If the
            if (player_ui.is_paused() && !player_ui.is_visible() && want_paused_clear) {
                renderer.clear();
                want_paused_clear = false;
            }

            // Suspend the thread for a frame worth of time
            // TODO: Wait for the display vsync event?
            std::this_thread::sleep_for(1e6us / 60);
        }
    }

    renderer.wait_idle();

    ImPlot::DestroyContext();
    ImGui::nx::exit();
    ImGui::DestroyContext();

    std::printf("Properly exiting\n");
    return 0;
}
