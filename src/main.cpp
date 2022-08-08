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

mpv_node node_map_find(mpv_node_list *l, std::string_view s) {
    for (int i = 0; i < l->num; ++i) {
        if (s == l->keys[i])
            return l->values[i];
    }
    return {};
};

} // namespace

int main(int argc, const char **argv) {
    if (argc < 2)
        argc = 2, argv[1] = "/Videos/akira60.mp4";

    std::printf("Starting player\n");

    ampnx::LibmpvController lmpv;
    if (lmpv.initialize()) {
        std::printf("Failed to initialize libmpv\n");
        return 1;
    }

    ImGui::CreateContext();
    ImPlot::CreateContext();

    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    imgui::nx::init();

    ampnx::Renderer renderer;
    if (renderer.initialize(lmpv)) {
        std::printf("Failed to initialize renderer\n");
        return 1;
    }

    lmpv.load_file(argv[1]);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    struct PassStats {
        std::string desc;
        std::array<ImS64, 256> samples;
        std::size_t num_samples = 0;
    };
    std::vector<PassStats> passes_stats{};

    std::string_view vo_passes_property = "vo-passes";
    bool enable_stats = false;
    lmpv.get_property_async<mpv_node>(vo_passes_property.data());

    std::int64_t vo_dropped = 0, dec_dropped = 0;
    while (appletMainLoop()) {
        mpv_event *mp_event = mpv_wait_event(lmpv.get_handle(), 0);
        switch (mp_event->event_id) {
            case MPV_EVENT_LOG_MESSAGE:
                do {
                    auto *msg = static_cast<mpv_event_log_message *>(mp_event->data);
                    std::printf("[%s]: %s", msg->prefix, msg->text);
                } while ((mp_event = mpv_wait_event(lmpv.get_handle(), 0)) && (mp_event->event_id == MPV_EVENT_LOG_MESSAGE));
                break;
            case MPV_EVENT_NONE:
                break;
            case MPV_EVENT_END_FILE:
                goto done;
            case MPV_EVENT_GET_PROPERTY_REPLY:
                {
                    auto *reply = static_cast<mpv_event_property *>(mp_event->data);
                    auto *node = static_cast<mpv_node *>(reply->data);

                    if (!mp_event->error && vo_passes_property == reply->name && reply->format == MPV_FORMAT_NODE && reply->data) {
                        auto *fresh = node_map_find(node->u.list, "fresh").u.list;
                        passes_stats.resize(fresh->num);

                        for (int i = 0; i < fresh->num; ++i) {
                            mpv_node_list *pass = fresh->values[i].u.list;
                            auto &stats = passes_stats[i];
                            stats.desc = node_map_find(pass, "desc").u.string;

                            auto *samples = node_map_find(pass, "samples").u.list;
                            for (int j = 0; j < samples->num; ++j)
                                stats.samples[j] = samples->values[j].u.int64/1000;
                            stats.num_samples = samples->num;
                        }
                    }
                    mpv_free_node_contents(node);
                    if (enable_stats)
                        lmpv.get_property_async<mpv_node>(vo_passes_property.data());
                }
                break;
            default:
                std::printf("[event]: %s\n", mpv_event_name(mp_event->event_id));
                break;
        }

        // std::int64_t new_vo_dropped, new_dec_dropped;
        // lmpv.get_property("frame-drop-count",         new_vo_dropped);
        // lmpv.get_property("decoder-frame-drop-count", new_dec_dropped);

        // if ((new_vo_dropped != vo_dropped) || (new_dec_dropped != dec_dropped)) {
        //     vo_dropped = new_vo_dropped, dec_dropped = new_dec_dropped;
        //     std::printf("Dropped frames: VO %ld, DEC %ld\n", vo_dropped, dec_dropped);
        // }

        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            goto done;

        if (padGetButtonsDown(&pad) & HidNpadButton_A)
            lmpv.command_async("cycle", "pause");

        if (padGetButtonsDown(&pad) & HidNpadButton_Y)
            lmpv.command_async("screenshot-to-file", "screenshot.png", "subtitles");

        if (padGetButtonsDown(&pad) & (HidNpadButton_R | HidNpadButton_L))
            lmpv.seek((padGetButtonsDown(&pad) & HidNpadButton_R) ? 5 : -5);

        if (padGetButtonsDown(&pad) & (HidNpadButton_ZR | HidNpadButton_ZL))
            lmpv.seek((padGetButtonsDown(&pad) & HidNpadButton_ZR) ? 60 : -60);

        HidTouchScreenState touch_state = {};
        imgui::nx::newFrame(&pad, hidGetTouchScreenStates(&touch_state, 1) ? &touch_state : nullptr);

        renderer.begin_frame();

        // ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_Once);
        if (ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Fps: %.1f, vtx: %d, idx: %d", io.Framerate, io.MetricsRenderVertices, io.MetricsRenderIndices);

            bool prev_enable_stats = enable_stats;
            if (ImGui::Checkbox("Enable stats", &enable_stats) && !prev_enable_stats)
                lmpv.get_property_async<mpv_node>(vo_passes_property.data());

            auto plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs |
                ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_AntiAliased;
            if (enable_stats && ImPlot::BeginPlot("fresh", ImVec2(600, 500), plot_flags)) {
                ImPlot::SetupAxes("", "µs", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_South, ImPlotLegendFlags_Outside);

                for (auto &stats: passes_stats)
                    ImPlot::PlotLine(stats.desc.c_str(), stats.samples.data(), stats.num_samples);

                ImPlot::EndPlot();
            }
        }
        ImGui::End();

        // if (ImGui::Begin("FPS"))
        //     ImGui::Text("UI: %.1f, vtx: %d, idx: %d", io.Framerate, io.MetricsRenderVertices, io.MetricsRenderIndices);
        // ImGui::End();

        renderer.end_frame();
    }

    imgui::nx::exit();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

done:
    std::printf("Properly exiting\n");
    return 0;
}
