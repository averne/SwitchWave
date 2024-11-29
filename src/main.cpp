// Copyright (c) 2024 averne <averne381@gmail.com>
//
// This file is part of SwitchWave.
//
// SwitchWave is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SwitchWave is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SwitchWave.  If not, see <http://www.gnu.org/licenses/>.

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string_view>
#include <thread>

#include <switch.h>

#include <imgui.h>
#include <implot.h>
#include <imgui_nx.h>

#include "libmpv.hpp"
#include "render.hpp"
#include "waves.hpp"
#include "context.hpp"
#include "ui/ui_main_menu.hpp"
#include "ui/ui_player.hpp"
#include "fs/fs_common.hpp"
#include "fs/fs_ums.hpp"
#include "fs/fs_recent.hpp"

using namespace std::chrono_literals;

namespace {

PadState            g_pad;
HidTouchScreenState g_touch_state;
std::mutex          g_setup_mtx;
bool                g_application_mode;

FsFileSystem        g_bis_user_fs;

extern "C" u32 __nx_applet_exit_mode, __nx_nv_service_type, __nx_nv_transfermem_size;

extern "C" void userAppInit(void) {
    // Keep the main thread above others so that the program stays responsive
    // when doing software decoding
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);

    appletLockExit();

    auto at = appletGetAppletType();
    g_application_mode = at == AppletType_Application || at == AppletType_SystemApplication;

    // To get access to /dev/nvhost-nvjpg, we need nvdrv:{a,s,t}
    // However, nvdrv:{a,s} have limited address space for gpu mappings
    __nx_nv_service_type     = NvServiceType_Factory;
    __nx_nv_transfermem_size = (g_application_mode ? 16 : 3) * 0x100000;

    hidsysInitialize();
    setsysInitialize();
    auddevInitialize();
    audctlInitialize();
    lblInitialize();
    inssInitialize();
    hidInitializeTouchScreen();

    fsOpenBisFileSystem(&g_bis_user_fs, FsBisPartitionId_User, "");
    romfsInit();

    // We need to connect to bsd:s to be able to bind sockets on ports < 1024,
    // as required for secure NFS connections
    auto socket_conf = *socketGetDefaultInitConfig();
    socket_conf.bsd_service_type = BsdServiceType_Auto;
    socketInitialize(&socket_conf);

#ifdef DEBUG
    nxlinkStdio();
#endif
}

extern "C" void userAppExit(void) {
    appletUnlockExit();

    fsFsClose(&g_bis_user_fs);
    romfsExit();

    hidsysExit();
    setsysExit();
    auddevExit();
    audctlExit();
    lblExit();
    inssExit();

    nvExit();

    socketExit();
}

void mpv_presetup() {
    auto lk = std::scoped_lock(g_setup_mtx);

    auto fonts_dir = sw::fs::Path(sw::LibmpvController::MpvDirectory) / "fonts";
    auto font_file = fonts_dir / "nintendo_udsg-r_std_003.ttf";

    if (std::filesystem::exists(font_file))
        return;

    if (!std::filesystem::exists(fonts_dir))
        std::filesystem::create_directories(fonts_dir);

    if (auto rc = plInitialize(PlServiceType_User); R_FAILED(rc))
        return;

    SW_SCOPEGUARD([] { plExit(); });

    PlFontData font;
    if (auto rc = plGetSharedFontByType(&font, PlSharedFontType_Standard); R_FAILED(rc))
        return;

    auto *fp = std::fopen(font_file.c_str(), "wb");
    if (!fp)
        return;
    SW_SCOPEGUARD([&fp] { std::fclose(fp); });

    std::fwrite(font.address, font.size, 1, fp);

    std::printf("Dumped standard font\n");
}

void ums_devices_changed_cb(const std::vector<sw::fs::UmsController::Device> &devices, void *user) {
    auto &context = *static_cast<sw::Context *>(user);

    // Remove unmounted devices
    for (auto &fs: context.filesystems) {
        if (fs->type != sw::fs::Filesystem::Type::Usb)
            continue;

        auto it = std::find_if(devices.begin(), devices.end(), [&fs](const auto &dev) {
            return fs->mount_name == dev.mount_name;
        });

        if (it == devices.end()) {
            std::erase(context.filesystems, fs);
            context.cur_fs = context.filesystems.front();
        }
    }

    // Add new devices
    for (auto &dev: devices) {
        auto it = std::find_if(context.filesystems.begin(), context.filesystems.end(), [&dev](const auto &fs) {
            return fs->mount_name == dev.mount_name;
        });

        if (it == context.filesystems.end())
            context.filesystems.emplace_back(std::make_shared<sw::fs::Filesystem>(
                sw::fs::Filesystem::Type::Usb, dev.name, dev.mount_name));
    }
}

int menu_loop(sw::Renderer &renderer, sw::Context &context) {
    renderer.switch_presentation_mode(false);

    context.cur_file.clear();

    auto waves = std::make_unique<sw::ui::Waves>      (renderer);
    auto menu  = std::make_unique<sw::ui::MainMenuGui>(renderer, context);

    while (!context.want_quit) {
        if (!appletMainLoop()) {
            context.want_quit = true;
            break;
        }

        padUpdate(&g_pad);
        auto has_touches = hidGetTouchScreenStates(&g_touch_state, 1);
        ImGui::nx::newFrame(&g_pad, has_touches ? &g_touch_state : nullptr);
        ImGui::NewFrame();

        if (!menu->update_state(g_pad, g_touch_state)) {
            ImGui::EndFrame();
            break;
        }

        renderer.begin_frame();
        waves->render();
        menu ->render();
        renderer.end_frame();
    }

    renderer.wait_idle();

    return 0;
}

int video_loop(sw::Renderer &renderer, sw::Context &context) {
    renderer.switch_presentation_mode(true);

    context.playback_started = context.player_is_idle = false;
    auto lmpv = sw::LibmpvController();

    lmpv.set_file_loaded_callback(+[](void *user) {
        static_cast<sw::Context *>(user)->playback_started = true;
    }, &context);

    lmpv.set_end_file_callback(+[](void *user, mpv_event_end_file *end) {
        if (end->reason == MPV_END_FILE_REASON_ERROR)
            static_cast<sw::Context *>(user)->last_error = end->error;
    }, &context);

    lmpv.set_idle_callback(+[](void *user) {
        auto *context = static_cast<sw::Context *>(user);
        if (context->playback_started)
            context->player_is_idle = true;
    }, &context);

#ifdef DEBUG
    lmpv.set_log_callback(+[](void*, mpv_event_log_message *msg) {
        std::printf("[%s]: %s", msg->prefix, msg->text);
    });
#endif

    if (auto rc = lmpv.initialize(); rc < 0) {
        std::printf("Failed to initialize libmpv\n");
        return rc;
    }

    if (auto rc = renderer.create_mpv_render_context(lmpv); rc < 0) {
        std::printf("Failed to initialize mpv render context\n");
        return rc;
    }

    auto lk = std::scoped_lock(g_setup_mtx);

    lmpv.command("loadfile", context.cur_file.c_str());

    auto player_ui = std::make_unique<sw::ui::PlayerGui>(renderer, context, lmpv);

    if (auto *fs = context.get_filesystem(sw::fs::Path::mountpoint(context.cur_file));
            fs && fs->type == sw::fs::Filesystem::Type::Network)
        lmpv.set_property_async("cache", "yes");

    if (!context.use_fast_presentation)
        renderer.switch_presentation_mode(false);

    bool old_ui_visible = false, want_paused_clear = false;
    while (!context.want_quit && !context.player_is_idle && !context.last_error) {
        if (!appletMainLoop()) {
            context.want_quit = true;
            break;
        }

        padUpdate(&g_pad);
        auto has_touches = hidGetTouchScreenStates(&g_touch_state, 1);
        ImGui::nx::newFrame(&g_pad, has_touches ? &g_touch_state : nullptr);
        ImGui::NewFrame();

        if (!player_ui->update_state(g_pad, g_touch_state)) {
            ImGui::EndFrame();
            break;
        }

        lmpv.process_events();

        bool ui_visible = player_ui->is_visible();
        if (context.use_fast_presentation && ui_visible != old_ui_visible) {
            renderer.switch_presentation_mode(!ui_visible);
            if (player_ui->is_paused())
                want_paused_clear = old_ui_visible == true;
            old_ui_visible = ui_visible;
        }

        if (!context.use_fast_presentation || ui_visible) {
            renderer.begin_frame();
            player_ui->render();
            renderer.end_frame();
        } else {
            if (player_ui->is_paused() && !player_ui->is_visible() && want_paused_clear) {
                renderer.begin_frame();
                renderer.end_frame();
                want_paused_clear = false;
            } else {
                ImGui::EndFrame();
            }

            // Suspend the thread for a frame worth of time
            // TODO: Wait for the display vsync event?
            std::this_thread::sleep_for(1e6us / 60);
        }
    }

    renderer.wait_idle();
    renderer.destroy_mpv_render_context();

    return context.last_error;
}

} // namespace

int main(int argc, const char **argv) {
    std::printf("Starting " APP_TITLE ", v" APP_VERSION ", built: " __DATE__ " " __TIME__ "\n");

    auto setup_thread = std::jthread(&mpv_presetup);

    hidSetNpadHandheldActivationMode(HidNpadHandheldActivationMode_Single);
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&g_pad);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::nx::init();

    SW_SCOPEGUARD([] {
        ImGui::nx::exit();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    });

    static sw::Renderer renderer;
    if (renderer.initialize()) {
        std::printf("Failed to initialize renderer\n");
        return 1;
    }

    static sw::Context context;
    if (context.read_from_file())
        std::printf("Failed to read configuration from file\n");

    if (!g_application_mode)
        context.set_error(-1, sw::Context::ErrorType::AppletMode);

    auto sdmc_fs = std::make_shared<sw::fs::Filesystem>(sw::fs::Filesystem::Type::Sdmc, "sdmc", "sdmc:");
    context.filesystems.emplace_back(sdmc_fs);

    if (serviceIsActive(&g_bis_user_fs.s) && (fsdevMountDevice("user", g_bis_user_fs) != -1)) {
        auto user_fs = std::make_shared<sw::fs::Filesystem>(sw::fs::Filesystem::Type::Sdmc, "user", "user:");
        context.filesystems.emplace_back(user_fs);
    }

    auto recent = std::make_shared<sw::fs::RecentFs>(context, "recent", "recent:");
    if (auto rc = recent->register_fs(); !rc)
        context.filesystems.emplace_back(recent);

    context.cur_fs = context.filesystems.front();

    if (auto rc = context.ums.initialize(); R_SUCCEEDED(rc))
        context.ums.set_devices_changed_callback(ums_devices_changed_cb, &context);
    else
        std::printf("Failed to initialize ums controller: %#x\n", rc);
    SW_SCOPEGUARD([] { context.ums.finalize(); });

    auto network_setup_thread = std::jthread([] {
        for (auto &info: context.network_infos) {
            if (info->want_connect) {
                if (auto rc = context.register_network_fs(*info); rc)
                    context.set_error(rc, sw::Context::ErrorType::Network);
            }
        }
    });

    if (argc > 1)
        context.cur_file = argv[1], context.cli_mode = true;

    while (!context.want_quit) {
        if (!context.cur_file.empty()) {
            if (auto rc = video_loop(renderer, context)) {
                std::printf("Failed to run player: %d (%s)\n", rc, mpv_error_string(rc));
                context.set_error(rc, sw::Context::ErrorType::Mpv);
            } else {
                recent->add(context.cur_file);
            }
        }

        if (context.cli_mode)
            break;

        if (auto rc = menu_loop(renderer, context))
            std::printf("Failed to run menu: %d\n", rc);
    }

    // Clear the screen before quitting
    ImGui::NewFrame();
    renderer.begin_frame();
    renderer.end_frame();

    if (recent->write_to_file())
        std::printf("Failed to write history to file\n");

    if (context.write_to_file())
        std::printf("Failed to write config to file\n");

    if (context.quit_to_home_menu && !context.cli_mode)
        __nx_applet_exit_mode = 1;

    std::printf("Properly exiting\n");
    return 0;
}
