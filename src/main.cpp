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

namespace {

extern "C" void userAppInit() {
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);

    appletLockExit();

    socketInitializeDefault();
    nxlinkStdio();

    plInitialize(PlServiceType_User);
    hidInitializeTouchScreen();
    romfsInit();
}

extern "C" void userAppExit() {
    socketExit();

    appletUnlockExit();

    plExit();
    romfsExit();
}

} // namespace

int main(int argc, const char **argv) {
    if (argc < 2)
        argc = 2, argv[1] = "/Videos/yryr.mkv";

    std::printf("Starting player\n");

    PadState pad;
    HidTouchScreenState touch_state;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    ampnx::LibmpvController lmpv;
    lmpv.set_log_callback([](void*, mpv_event_log_message *msg) {
        std::printf("[%s]: %s", msg->prefix, msg->text);
    });
    if (lmpv.initialize()) {
        std::printf("Failed to initialize libmpv\n");
        return 1;
    }

    ImGui::CreateContext();
    ImPlot::CreateContext();

    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::nx::init();

    ampnx::Renderer renderer;
    if (renderer.initialize(lmpv)) {
        std::printf("Failed to initialize renderer\n");
        return 1;
    }

    ampnx::PlayerGui player_ui(renderer, lmpv);

    // lmpv.observe_property<std::int64_t *>("frame-drop-count", nullptr, +[](void*, mpv_event_property *prop) {
    //     std::printf("VO dropped: %ld\n", *static_cast<std::int64_t *>(prop->data));
    // });

    lmpv.load_file(argv[1]);

    while (appletMainLoop()) {
        padUpdate(&pad);
        auto has_touches = hidGetTouchScreenStates(&touch_state, 1);

        lmpv.process_events();

        if (!player_ui.update_state(pad, touch_state))
            break;

        ImGui::nx::newFrame(&pad, has_touches ? &touch_state : nullptr);

        renderer.begin_frame();

        player_ui.render();

        renderer.end_frame();
    }

    renderer.wait_idle();

    ImGui::nx::exit();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    std::printf("Properly exiting\n");
    return 0;
}
