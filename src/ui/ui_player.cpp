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

#include <algorithm>
#include <format>
#include <tuple>
#include <utility>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <imgui_nx.h>
#include <imgui_deko3d.h>

#include "utils.hpp"
#include "ui/ui_explorer.hpp"

#include "ui/ui_player.hpp"

namespace sw::ui {

using namespace std::chrono_literals;

namespace {

#define FORMAT_TIME(s) (s)/60/60%99, (s)/60%60, (s)%60

Result get_active_audio_target(AudioTarget &target) {
    if (hosversionAtLeast(13,0,0)) {
        return audctlGetActiveOutputTarget(&target);
    } else {
        AudioDeviceName dev;
        if (auto rc = auddevGetActiveAudioDeviceName(&dev); R_FAILED(rc))
            return rc;

        auto n = std::string_view(dev.name);
        if (n == "AudioBuiltInSpeakerOutput")
            target = AudioTarget_Speaker;
        else if (n == "AudioStereoJackOutput")
            target = AudioTarget_Headphone;
        else if (n == "AudioTvOutput")
            target = AudioTarget_Tv;
        else
            target = AudioTarget_Invalid;

        return 0;
    }
}

constexpr HidTouchState *find_touch_by_id(HidTouchScreenState &state, std::uint32_t id) {
    for (std::size_t i = 0; i < SW_ARRAYSIZE(state.touches); ++i) {
        auto &t = state.touches[i];
        if (t.finger_id == id)
            return &t;
        if (t.finger_id > id)
            return nullptr;
    }
    return nullptr;
}

constexpr std::pair<std::int32_t, std::int32_t> touch_delta(HidTouchState &a, HidTouchState &b) {
    return std::pair{ std::int32_t(a.x - b.x), std::int32_t(a.y - b.y) };
}

constexpr float touch_distance(HidTouchState &a, HidTouchState &b) {
    auto [dx, dy] = touch_delta(a, b);
    return std::sqrt(dx*dx + dy*dy);
}

} // namespace

void PlayerGui::screenshot_button_thread_fn(std::stop_token token) {
    // This needs a high priority because we are racing am to clear the event
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);

    Event screenshot_evt;
    SW_SCOPEGUARD([&screenshot_evt] { eventClose(&screenshot_evt); });
    if (auto rc = hidsysAcquireCaptureButtonEventHandle(&screenshot_evt, false); R_FAILED(rc)) {
        std::printf("Failed to acquire the screenshot button event: %#x\n", rc);
        return;
    }

    Event activity_evt;
    SW_SCOPEGUARD([&activity_evt] { eventClose(&activity_evt); });
    if (auto rc = inssGetWritableEvent(0, &activity_evt); R_FAILED(rc)) {
        std::printf("Failed to acquire the activity event: %#x\n", rc);
        return;
    }

    UTimer activity_timer;
    SW_SCOPEGUARD([&activity_timer] { utimerStop(&activity_timer); });

    utimerCreate(&activity_timer, std::chrono::nanoseconds(500ms).count(), TimerType_Repeating);
    utimerStart(&activity_timer);

    eventClear(&screenshot_evt);

    u64 down_start_tick = 0;
    bool has_captured_movie = false;
    while (!token.stop_requested()) {
        s32 idx;
        auto rc = waitMulti(&idx, std::chrono::nanoseconds(50ms).count(),
            waiterForEvent(&screenshot_evt), waiterForUTimer(&activity_timer));

        if (!this->context.override_screenshot_button && !this->context.disable_screensaver)
            continue;

        auto delta = armTicksToNs(armGetSystemTick() - down_start_tick);
        if (down_start_tick && delta >= PlayerGui::MovieCaptureTimeout && !has_captured_movie) {
            appletPerformSystemButtonPressingIfInFocus(AppletSystemButtonType_CaptureButtonLongPressing);
            has_captured_movie = true;
        }

        if (rc == KERNELRESULT(TimedOut))
            continue;

        switch (idx) {
            case 0: // Screenshot button
                if (!this->context.override_screenshot_button)
                    break;

                eventClear(&screenshot_evt);

                if (!down_start_tick) {
                    down_start_tick = armGetSystemTick();
                } else {
                    if (delta < PlayerGui::MovieCaptureTimeout) {
                        this->lmpv.command_async("screenshot", "subtitles");
                        this->set_show_string(500ms, "Saving screenshot");
                    }

                    down_start_tick    = 0;
                    has_captured_movie = false;
                }
                break;
            case 1: // Activity report timer
                if (!this->context.disable_screensaver)
                    break;

                // First class support for Fizeau, which detects inactivity through ins:r event 0
                eventFire(&activity_evt);
                break;
            default:
                break;
        }
    }
}

PlayerGui::PlayerGui(Renderer &renderer, Context &context, LibmpvController &lmpv):
        Widget(renderer), lmpv(lmpv), context(context),
        seek_bar(renderer, context, lmpv), menu(renderer, context, lmpv), console(renderer, lmpv) {
    this->screenshot_button_thread = std::jthread(&PlayerGui::screenshot_button_thread_fn, this);
}

PlayerGui::~PlayerGui() {
    appletSetMediaPlaybackState(false);

    this->screenshot_button_thread.request_stop();
}

bool PlayerGui::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto now  = std::chrono::system_clock::now();
    auto buttons = padGetButtons(&pad), down = padGetButtonsDown(&pad);

    if (down & HidNpadButton_Plus && !ImGui::nx::isSwkbdVisible())
        return false;

    // Can only run when the swkbd isn't shown so don't bother using ImGui API
    if (!(this->menu.is_visible || this->console.is_visible)) {
        if (!this->seek_bar.is_visible && (down & (HidNpadButton_A | HidNpadButton_X)))
            this->lmpv.set_property_async("pause", int(!this->seek_bar.pause));

        if (buttons & SeekBar::SeekBarPopButtons)
            this->seek_bar.begin_visible();

        if ((buttons & (HidNpadButton_Up | HidNpadButton_Down)) &&
                (down & (HidNpadButton_ZL | HidNpadButton_ZR))) {
            printf("Chapter: %ld\n", this->seek_bar.chapter);
            auto chapter = this->seek_bar.chapter;
            if (down & HidNpadButton_ZL)
                chapter -= 1;
            if (down & HidNpadButton_ZR)
                chapter += 1;
            this->lmpv.set_property_async("chapter", chapter);
        }
    }

    if (down & (HidNpadButton_StickL | HidNpadButton_StickR)) {
        this->lmpv.command_async("screenshot", "subtitles");
        this->set_show_string(500ms, "Saving screenshot");
    }

    if (!ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) && !ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp)) {
        constexpr static std::array key_seek_map = {
            std::pair{ImGuiKey_GamepadL1, -05.0},
            std::pair{ImGuiKey_GamepadL2, -60.0},
            std::pair{ImGuiKey_GamepadR1, +05.0},
            std::pair{ImGuiKey_GamepadR2, +60.0},
        };

        for (auto &&[key, time]: key_seek_map) {
            if (ImGui::IsKeyPressed(key))
                this->lmpv.set_property_async("time-pos", this->seek_bar.time_pos + time);
        }
    }

    auto &io = ImGui::GetIO();
    auto &js_lleft = io.KeysData[ImGuiKey_GamepadLStickLeft], &js_lright = io.KeysData[ImGuiKey_GamepadLStickRight],
         &js_rleft = io.KeysData[ImGuiKey_GamepadRStickLeft], &js_rright = io.KeysData[ImGuiKey_GamepadRStickRight],
         &js_rup   = io.KeysData[ImGuiKey_GamepadRStickUp],   &js_rdown  = io.KeysData[ImGuiKey_GamepadRStickDown];

    if (!(this->menu.is_visible || this->console.is_visible) && (js_lleft.Down || js_lright.Down)) {
        if (this->js_time_start == 0.0)
            this->js_time_start = this->seek_bar.time_pos;

        auto percent_pos = this->seek_bar.percent_pos + (-js_lleft.AnalogValue + js_lright.AnalogValue) / 3.0f;
        this->set_show_string(1s, "%02u:%02u:%02u (%+.1fs)",
            FORMAT_TIME(std::uint32_t(this->seek_bar.duration * percent_pos / 100.0)), this->seek_bar.time_pos - this->js_time_start);
        this->lmpv.set_property_async("percent-pos", percent_pos);
    } else {
        this->js_time_start = 0.0;
    }

    if ((js_rup.Down || js_rdown.Down) && (now - this->last_brightness_change > PlayerGui::BrightnessVolumeChangeTimeout)) {
        float brightness;
        auto rc = lblGetCurrentBrightnessSetting(&brightness);

        auto tmp = int(brightness * 10) + (((-js_rdown.AnalogValue + js_rup.AnalogValue) > 0.0f) ? 1 : -1);
        brightness = float(std::clamp(tmp, 0, 10)) / 10.0f;

        if (R_SUCCEEDED(rc))
            rc = lblSetCurrentBrightnessSetting(brightness);

        if (R_SUCCEEDED(rc))
            this->set_show_string(1s, "Brightness: %.0f%%", brightness * 100.0f);
        else
            std::printf("Failed to set brightness: %#x\n", rc);

        this->last_brightness_change = now;
    }

    if ((js_rleft.Down || js_rright.Down) && (now - this->last_volume_change > PlayerGui::BrightnessVolumeChangeTimeout)) {
        AudioTarget target;
        auto rc = get_active_audio_target(target);

        s32 vol;
        if (R_SUCCEEDED(rc))
            rc = audctlGetTargetVolume(&vol, target);

        vol += ((-js_rleft.AnalogValue + js_rright.AnalogValue) > 0.0f) ? 1 : -1;
        vol = std::clamp(vol, 0, 15);

        if (R_SUCCEEDED(rc))
            rc = audctlSetTargetVolume(target, vol);

        if (R_SUCCEEDED(rc))
            this->set_show_string(1s, "Volume: %d%%", vol * 100 / 15);
        else
            std::printf("Failed to set volume: %#x\n", rc);

        this->last_volume_change = now;
    }

    if (touch.count) {
        if (this->has_touch) {
            auto *t = find_touch_by_id(touch, this->orig_touch.finger_id);
            this->has_touch = !!t;
            if (t)
                this->cur_touch = *t;
        }

        if (!this->has_touch) {
            this->touch_state = TouchGestureState::Tap;
            this->cur_touch = this->orig_touch = touch.touches[0];
            this->has_touch = true;
        }
    } else {
        if (this->has_touch && this->touch_state == TouchGestureState::Tap &&
                !this->seek_bar.ignore_input)
            this->seek_bar.begin_visible();

        this->has_touch = false;
    }

    if (this->has_touch && !(this->menu.is_visible || this->console.is_visible)) {
        auto d = touch_distance(this->cur_touch, this->orig_touch);
        auto [dx, dy] = touch_delta(this->cur_touch, this->orig_touch);
        auto sdx = float(dx) / this->renderer.image_width,
             sdy = float(dy) / this->renderer.image_height;

        if ((this->touch_state == TouchGestureState::Tap) && (d >= PlayerGui::TouchGestureThreshold)) {
            if (std::abs(dx) >= std::abs(dy)) {
                if (this->renderer.image_height - this->orig_touch.y > this->screen_rel_height(SeekBar::BarHeight)) {
                    this->touch_state = TouchGestureState::SlideSeek;
                    this->touch_setting_start.time_pos = this->seek_bar.time_pos;
                }
            } else if (this->orig_touch.x < this->renderer.image_width / 2) {
                float brightness;
                if (auto rc = lblGetCurrentBrightnessSetting(&brightness); R_SUCCEEDED(rc)) {
                    this->touch_state = TouchGestureState::SlideBrightness;
                    this->touch_setting_start.brightness = brightness;
                }
            } else {
                AudioTarget target;
                auto rc = get_active_audio_target(target);

                s32 vol;
                if (R_SUCCEEDED(rc))
                    rc = audctlGetTargetVolume(&vol, target);

                if (R_SUCCEEDED(rc)) {
                    this->touch_state = TouchGestureState::SlideVolume;
                    this->touch_setting_start.audio_target = target;
                    this->touch_setting_start.audio_vol    = vol;
                }
            }

            // The threshold shouldn't count towards the distance from origin
            this->orig_touch = this->cur_touch;
        }

        switch (this->touch_state) {
            default:
            case TouchGestureState::Tap:
                break;
            case TouchGestureState::SlideSeek: {
                    auto time_pos = PlayerGui::TouchGestureXMultipler * sdx;
                    this->set_show_string(1s, "%02u:%02u:%02u (%+.1fs)",
                        FORMAT_TIME(std::uint32_t(this->touch_setting_start.time_pos + time_pos)), time_pos);
                    this->lmpv.set_property_async("time-pos", this->touch_setting_start.time_pos + time_pos);
                }
                break;
            case TouchGestureState::SlideBrightness: {
                    auto brightness = this->touch_setting_start.brightness -
                        PlayerGui::TouchGestureYMultipler * sdy;
                    brightness = std::clamp(brightness, 0.0f, 1.0f);

                    if (auto rc = lblSetCurrentBrightnessSetting(brightness); R_SUCCEEDED(rc))
                        this->set_show_string(1s, "Brightness: %.0f%%", brightness * 100.0f);
                    else
                        std::printf("Failed to set brightness: %#x\n", rc);
                }
                break;
            case TouchGestureState::SlideVolume: {
                    auto vol = this->touch_setting_start.audio_vol -
                        int(PlayerGui::TouchGestureYMultipler * sdy * 15);
                    vol = std::clamp(vol, 0, 15);

                    if (auto rc = audctlSetTargetVolume(this->touch_setting_start.audio_target, vol); R_SUCCEEDED(rc))
                        this->set_show_string(1s, "Volume: %d%%", vol * 100 / 15);
                    else
                        std::printf("Failed to set volume: %#x\n", rc);
                }
                break;
        }
    }

    if (now - this->show_string_begin > this->show_string_timeout)
        this->has_show_string = false;

    this->seek_bar.update_state(pad, touch);
    if (!this->console.is_visible)
        this->menu.update_state(pad, touch);
    if (!this->menu.is_visible)
        this->console.update_state(pad, touch);

    this->seek_bar.ignore_input = this->menu.is_visible || this->console.is_visible;

    return true;
}

void PlayerGui::render() {
    auto &imstyle = ImGui::GetStyle();
    imstyle.Alpha = 0.8f;

    this->menu    .render();
    this->seek_bar.render();
    this->console .render();

    if (this->has_show_string) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        SW_SCOPEGUARD([] { ImGui::PopStyleColor(); ImGui::PopStyleColor(); });

        ImGui::Begin("##showstringwin", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::SetWindowPos (ImVec2(0.0f, 0.0f));
        ImGui::SetWindowFontScale(1.5f * this->scale_factor());
        SW_SCOPEGUARD([] { ImGui::End(); });

        ImGui::TextColored(ImColor(1.0f, 1.0f, 1.0f, 1.0f), "%s", this->show_string.data());
    }
}

SeekBar::SeekBar(Renderer &renderer, Context &context, LibmpvController &lmpv):
        Widget(renderer), lmpv(lmpv), context(context) {
    this->play_texture      = this->renderer.load_texture("romfs:/textures/play-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->pause_texture     = this->renderer.load_texture("romfs:/textures/pause-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->previous_texture  = this->renderer.load_texture("romfs:/textures/previous-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->next_texture      = this->renderer.load_texture("romfs:/textures/next-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);

    this->lmpv.observe_property("pause", &this->pause, +[](void *user, mpv_event_property *prop) {
        auto *self   = static_cast<SeekBar *>(user);
        auto *paused = static_cast<int     *>(prop->data);

        if (!self->context.disable_screensaver)
            return;

        if (auto rc = appletSetMediaPlaybackState(!*paused); R_FAILED(rc))
            std::printf("Failed to set media playback state: %#x\n", rc);
    }, this);

    this->lmpv.observe_property("time-pos",    &this->time_pos);
    this->lmpv.observe_property("duration",    &this->duration);
    this->lmpv.observe_property("percent-pos", &this->percent_pos);
    this->lmpv.observe_property("chapter",     &this->chapter);
    this->lmpv.observe_property("media-title", &this->media_title);

    this->lmpv.observe_property("chapter-list", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self     = static_cast<SeekBar  *>(user);
        auto *node     = static_cast<mpv_node *>(prop->data);
        auto *chapters = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        self->chapters.resize(chapters->num);

        for (int i = 0; i < chapters->num; ++i) {
            auto *chapter = chapters->values[i].u.list;

            self->chapters[i] = {
                .title = LibmpvController::node_map_find<char *>(chapter, "title"),
                .time  = LibmpvController::node_map_find<double>(chapter, "time" ),
            };
        }
    }, this);

    this->lmpv.observe_property("demuxer-cache-state", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self        = static_cast<SeekBar  *>(user);
        auto *node        = static_cast<mpv_node *>(prop->data);
        auto *cache_state = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        auto *ranges = LibmpvController::node_map_find<mpv_node_list *>(cache_state, "seekable-ranges");

        self->seekable_ranges.resize(ranges->num);

        for (int i = 0; i < ranges->num; ++i) {
            auto *range = ranges->values[i].u.list;

            self->seekable_ranges[i] = {
                .start = LibmpvController::node_map_find<double>(range, "start"),
                .end   = LibmpvController::node_map_find<double>(range, "end"  ),
            };
        }
    }, this);
}

SeekBar::~SeekBar() {
    this->lmpv.unobserve_property("pause");
    this->lmpv.unobserve_property("time-pos");
    this->lmpv.unobserve_property("duration");
    this->lmpv.unobserve_property("percent-pos");
    this->lmpv.unobserve_property("chapter");
    this->lmpv.unobserve_property("chapter-list");
    this->lmpv.unobserve_property("demuxer-cache-state");
    this->lmpv.unobserve_property("media-title");

    this->renderer.unregister_texture(this->play_texture);
    this->renderer.unregister_texture(this->pause_texture);
    this->renderer.unregister_texture(this->next_texture);
    this->renderer.unregister_texture(this->previous_texture);
}

bool SeekBar::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto now  = std::chrono::system_clock::now();

    if (this->is_visible && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        this->is_visible = false, this->fadeio_alpha = 0.0f;

    if (this->is_visible) {
        auto delta = std::chrono::duration_cast<FloatUS>(now - this->visible_start);

        if (this->fadeio_alpha != 1.0f && delta < SeekBar::VisibleFadeIO) {
            this->fadeio_alpha = delta / SeekBar::VisibleFadeIO;
            this->is_appearing = true;
        } else if (delta > SeekBar::VisibleDelay - SeekBar::VisibleFadeIO) {
            this->fadeio_alpha = (SeekBar::VisibleDelay - delta) / SeekBar::VisibleFadeIO;
        } else {
            this->fadeio_alpha = 1.0f;
            this->is_appearing = false;
        }

        if (delta >= SeekBar::VisibleDelay)
            this->is_visible = false, this->fadeio_alpha = 0.0f;
    }

    return false;
}

void SeekBar::render() {
    if (!this->is_visible)
        return;

    auto &io    =  ImGui::GetIO();
    auto &style =  ImGui::GetStyle();
    auto &imctx = *ImGui::GetCurrentContext();

    imctx.NavDisableHighlight = false;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,            this->fadeio_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    SW_SCOPEGUARD([] { ImGui::PopStyleVar(); });
    SW_SCOPEGUARD([] { ImGui::PopStyleVar(); });

    ImGui::Begin("##seekbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(SeekBar::BarWidth, SeekBar::BarHeight));
    ImGui::SetWindowPos (this->screen_rel_vec<ImVec2>((1.0 - SeekBar::BarWidth) / 2.0, 1.0 - SeekBar::BarHeight));
    SW_SCOPEGUARD([] { ImGui::End(); });

    ImGui::SetWindowFontScale(0.8 * this->scale_factor());
    if (auto *chap = this->get_current_chapter(); chap && !chap->title.empty())
        ImGui::Text("%s - %s", chap->title.c_str(), this->media_title);
    else
        ImGui::Text(this->media_title ?: "");

    ImGui::SetWindowFontScale(this->scale_factor());

    auto win_cursor = ImGui::GetCursorPos();
    auto avail = ImGui::GetWindowSize() - win_cursor;

    auto imagebtn_padding = 2.0f * (style.FramePadding.y + style.FrameBorderSize);
    auto img_size = avail.y - 2*imagebtn_padding - style.FramePadding.y;
    ImGui::SetCursorPosY(win_cursor.y + style.FramePadding.y);

    auto img_handle = ImGui::deko3d::makeTextureID(
        (this->pause ? this->play_texture : this->pause_texture).handle, true);
    ImColor tint_col = (ImGui::nx::getCurrentTheme() == ColorSetId_Dark) ?
        ImVec4(1, 1, 1, this->fadeio_alpha) : ImVec4(0, 0, 0, this->fadeio_alpha);

    // Play/pause/skip buttons
    if (ImGui::ImageButton("##playpause", img_handle, ImVec2(img_size, img_size),
            ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint_col))
        this->lmpv.set_property_async("pause", int(!this->pause));
    if (this->is_appearing) {
        imctx.NavWindow = imctx.CurrentWindow;
        ImGui::SetNavID(ImGui::GetItemID(), imctx.NavLayer, 0, ImRect());
    }

    ImGui::SameLine();
    if (ImGui::ImageButton("##previousbtn", ImGui::deko3d::makeTextureID(this->previous_texture.handle, true),
            ImVec2{img_size, img_size}, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint_col)) {
        this->lmpv.command_async("playlist-prev", "weak");
    }

    ImGui::SameLine();
    if (ImGui::ImageButton("##nextbtn",     ImGui::deko3d::makeTextureID(this->next_texture.handle,     true),
            ImVec2{img_size, img_size}, ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint_col)) {
        this->lmpv.command_async("playlist-next", "weak");
    }

    ImGui::SameLine(); ImGui::SetCursorPosY(win_cursor.y);
    auto text_ypos = win_cursor.y + (avail.y - ImGui::GetFontSize()) / 2.0f;

    // Current timestamp
    ImGui::SetCursorPosY(text_ypos);
    ImGui::Text("%02u:%02u:%02u", FORMAT_TIME(std::uint32_t(this->time_pos)));

    // Seek bar
    ImGui::SameLine(); ImGui::SetCursorPosY(win_cursor.y + std::floor(SeekBar::SeekBarContourPx / 2));
    auto seekbar_padding = ImVec2(this->screen_rel_height(SeekBar::SeekBarPadding),
        this->screen_rel_height(SeekBar::SeekBarPadding));
    auto scr_cursor  = ImGui::GetCursorScreenPos();
    auto bb          = ImRect(scr_cursor, scr_cursor + ImVec2(this->screen_rel_width(SeekBar::SeekBarWidth), avail.y - SeekBar::SeekBarContourPx + 1));
    auto interior_bb = ImRect(bb.Min + seekbar_padding, bb.Max - seekbar_padding);

    ImGui::ItemSize(bb);
    ImGui::ItemAdd(bb, ImGui::GetID("##seekbar"), nullptr, ImGuiItemFlags_Disabled);

    auto ts_to_seekbar_pos = [this, &interior_bb](double timestamp) {
        return std::round(interior_bb.Min.x +
            interior_bb.GetWidth() * float(timestamp) / float(this->duration));
    };

    auto seekbar_pos_to_ts = [this, &interior_bb](float x) {
        return (x - interior_bb.Min.x) / interior_bb.GetWidth() * float(this->duration);
    };

    if (io.MouseDown[0] && interior_bb.Contains(io.MousePos) && interior_bb.Contains(io.MouseClickedPos[0])) {
        this->begin_visible();
        this->lmpv.set_property_async("time-pos", double(seekbar_pos_to_ts(io.MousePos.x)));
    }

    auto *list = ImGui::GetWindowDrawList();
    list->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_Button), 0, 0, SeekBar::SeekBarContourPx);

    // Avoid using AddRectFilled to get subpixel vertex positioning
    list->PathRect(interior_bb.Min,
        ImVec2{interior_bb.Min.x + interior_bb.GetWidth() * float(this->percent_pos) / 100.0f, interior_bb.Max.y});
    list->PathFillConvex(ImGui::GetColorU32(ImGuiCol_ButtonActive));

    // Chapters
    for (auto &chapter: this->chapters) {
        // Skip the first chapter
        if (chapter.time == 0.0)
            continue;

        auto pos_x = ts_to_seekbar_pos(chapter.time);
        list->AddLine(ImVec2{pos_x, interior_bb.Min.y},
            ImVec2{pos_x, interior_bb.Max.y}, tint_col, SeekBar::SeekBarLinesWidthPx);
    }

    auto pos_y = interior_bb.GetCenter().y;
    for (auto &range: this->seekable_ranges) {
        list->AddLine(ImVec2{ts_to_seekbar_pos(range.start), pos_y},
            ImVec2{ts_to_seekbar_pos(range.end), pos_y}, tint_col, SeekBar::SeekBarLinesWidthPx);
    }

    // Total duration
    ImGui::SameLine(); ImGui::SetCursorPosY(text_ypos);
    ImGui::Text("%02u:%02u:%02u", FORMAT_TIME(std::uint32_t(this->duration)));
}

PlayerMenu::PlayerMenu(Renderer &renderer, Context &context, LibmpvController &lmpv):
        Widget(renderer), lmpv(lmpv), context(context), explorer(renderer, context) {
    this->lmpv.observe_property("track-list", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self   = static_cast<PlayerMenu *>(user);
        auto *node   = static_cast<mpv_node *>(prop->data);
        auto *tracks = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        auto disable_track = TrackInfo{
            .name     = "None",
            .track_id = 0,
            .selected = false,
        };

        self->video_tracks.clear(), self->video_tracks.push_back(disable_track);
        self->audio_tracks.clear(), self->audio_tracks.push_back(disable_track);
        self->sub_tracks  .clear(), self->sub_tracks  .push_back(disable_track);

        for (int i = 0; i < tracks->num; ++i) {
            auto *track = tracks->values[i].u.list;

            auto  id    = LibmpvController::node_map_find<std::int64_t>(track, "id");
            auto *title = LibmpvController::node_map_find<char *>      (track, "title");
            auto *lang  = LibmpvController::node_map_find<char *>      (track, "lang");

            std::string name;
            if (title && lang)
                name = std::format("{} ({})", title, lang);
            else if (title)
                name = title;
            else if (lang)
                name = lang;
            else
                name = std::format("[Unnamed {:02d}]", id);

            name += "##" + std::to_string(id);

            auto info = TrackInfo{
                .name     =   std::move(name),
                .track_id =   id,
                .selected = !!LibmpvController::node_map_find<std::uint32_t>(track, "selected"),
            };

            std::string_view type = LibmpvController::node_map_find<char *>(track, "type");
            if (type == "video")
                self->video_tracks.push_back(std::move(info));
            else if (type == "audio")
                self->audio_tracks.push_back(std::move(info));
            else if (type == "sub")
                self->sub_tracks  .push_back(std::move(info));
        }

        auto is_any_track_selected = [](const std::vector<TrackInfo> &tracks) {
            return std::any_of(tracks.begin(), tracks.end(), [](auto &track) { return track.selected; });
        };

        self->video_tracks[0].selected = !is_any_track_selected(self->video_tracks);
        self->audio_tracks[0].selected = !is_any_track_selected(self->audio_tracks);
        self->sub_tracks  [0].selected = !is_any_track_selected(self->sub_tracks);
    }, this);

    this->lmpv.observe_property("playlist", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self     = static_cast<PlayerMenu *>(user);
        auto *node     = static_cast<mpv_node *>(prop->data);
        auto *playlist = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        self->playlist_info.clear();

        for (int i = 0; i < playlist->num; ++i) {
            auto *entry = playlist->values[i].u.list;

            auto *filename = LibmpvController::node_map_find<char *>(entry, "filename"),
                 *title    = LibmpvController::node_map_find<char *>(entry, "title");

            auto track_info = PlaylistEntryInfo{
                .name    =   title ?: fs::Path::filename(filename).data(),
                .id      =   LibmpvController::node_map_find<std::int64_t> (entry, "id"),
                .playing = !!LibmpvController::node_map_find<std::uint32_t>(entry, "current")
            };

            self->playlist_info.emplace_back(std::move(track_info));
        }
    }, this);

    this->last_stats_update = std::chrono::system_clock::now();

    this->lmpv.observe_property("file-format",      &this->file_format);
    this->lmpv.observe_property("video-codec",      &this->video_codec);
    this->lmpv.observe_property("audio-codec",      &this->audio_codec);
    this->lmpv.observe_property("hwdec-current",    &this->hwdec_current, +[](void *user, mpv_event_property *prop) {
        auto *self  = static_cast<MpvOptionCheckbox *>(user);
        self->value = std::string_view(*static_cast<char **>(prop->data)) != "no";
    }, &this->use_hwdec_checkbox);
    this->lmpv.observe_property("hwdec-interop",    &this->hwdec_interop);
    this->lmpv.observe_property("avsync",           &this->avsync);
    this->lmpv.observe_property("frame-drop-count", &this->dropped_vo_frames,
#ifdef DEBUG
        +[](void*, mpv_event_property *prop) {
            std::printf("VO  dropped: %ld\n", *static_cast<std::int64_t *>(prop->data));
        }
#else
        nullptr
#endif
    );
    this->lmpv.observe_property("decoder-frame-drop-count", &this->dropped_dec_frames,
#ifdef DEBUG
        +[](void*, mpv_event_property *prop) {
            std::printf("DEC dropped: %ld\n", *static_cast<std::int64_t *>(prop->data));
        }
#else
    nullptr
#endif
    );
    this->lmpv.observe_property("video-bitrate",    &this->video_bitrate);
    this->lmpv.observe_property("audio-bitrate",    &this->audio_bitrate);
    this->lmpv.observe_property("container-fps",    &this->container_specified_fps);
    this->lmpv.observe_property("estimated-vf-fps", &this->container_estimated_fps);
    this->lmpv.observe_property("video-unscaled",   &this->video_unscaled);
    this->lmpv.observe_property("keepaspect",       &this->keepaspect);

    this->lmpv.observe_property("video-params", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self   = static_cast<PlayerMenu *>(user);
        auto *node   = static_cast<mpv_node *>(prop->data);
        auto *params = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        self->video_width       = LibmpvController::node_map_find<std::int64_t>(params, "w");
        self->video_height      = LibmpvController::node_map_find<std::int64_t>(params, "h");
        self->video_pixfmt      = LibmpvController::node_map_find<char *>(params, "pixelformat")    ?: "";
        self->video_hw_pixfmt   = LibmpvController::node_map_find<char *>(params, "hw-pixelformat") ?: "";
        self->video_colorspace  = LibmpvController::node_map_find<char *>(params, "colormatrix")    ?: "";
        self->video_color_range = LibmpvController::node_map_find<char *>(params, "colorlevels")    ?: "";
        self->video_gamma       = LibmpvController::node_map_find<char *>(params, "gamma")          ?: "";
    }, this);

    this->lmpv.observe_property("osd-dimensions", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self = static_cast<PlayerMenu *>(user);
        auto *node = static_cast<mpv_node *>(prop->data);
        auto *dims = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        self->video_width_scaled   = LibmpvController::node_map_find<std::int64_t>(dims, "w") -
            LibmpvController::node_map_find<std::int64_t>(dims, "ml") -
            LibmpvController::node_map_find<std::int64_t>(dims, "mr");
        self->video_height_scaled  = LibmpvController::node_map_find<std::int64_t>(dims, "h") -
            LibmpvController::node_map_find<std::int64_t>(dims, "mt") -
            LibmpvController::node_map_find<std::int64_t>(dims, "mb");
    }, this);

    this->lmpv.observe_property("audio-params", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self   = static_cast<PlayerMenu *>(user);
        auto *node   = static_cast<mpv_node *>(prop->data);
        auto *params = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        self->audio_format       = LibmpvController::node_map_find<char *>(params, "format")   ?: "";
        self->audio_layout       = LibmpvController::node_map_find<char *>(params, "channels") ?: "";
        self->audio_samplerate   = LibmpvController::node_map_find<std::int64_t>(params, "samplerate");
        self->audio_num_channels = LibmpvController::node_map_find<std::int64_t>(params, "channel-count");
    }, this);

    this->lmpv.observe_property("profile-list", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self     = static_cast<PlayerMenu *>(user);
        auto *node     = static_cast<mpv_node *>(prop->data);
        auto *profiles = node->u.list;

        SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

        auto hash_str = [](std::string_view str) {
            uint32_t res = 0x1e4b293;
            for (std::size_t i = 0; i < str.size(); ++i)
                res ^= str[i] << (i & 0x1f);
            return res;
        };

        constexpr static std::array profile_blacklist = {
            hash_str("opengl-hq"),
            hash_str("libmpv"),
            hash_str("pseudo-gui"),
            hash_str("builtin-pseudo-gui"),
            hash_str("sw-fast"),
            hash_str("encoding"),
        };

        self->profile_list.clear();

        for (int i = 0; i < profiles->num; ++i) {
            auto *profile = profiles->values[i].u.list;

            auto *name = LibmpvController::node_map_find<char *>(profile, "name");
            if (!name)
                continue;

            auto it = std::find(profile_blacklist.begin(), profile_blacklist.end(), hash_str(name));
            if (it != profile_blacklist.end())
                continue;

            self->profile_list.emplace_back(name);
        }

        std::sort(self->profile_list.begin(), self->profile_list.end());
    }, this);

    this->lmpv.get_property_async("container-fps", &this->sub_fps_combo.options[0].second);

    this->fbo_format_combo       .observe(this->lmpv);
    this->hdr_peak_checkbox      .observe(this->lmpv);
    this->deinterlace_checkbox   .observe(this->lmpv);
    this->aspect_ratio_combo     .observe(this->lmpv);
    this->rotation_combo         .observe(this->lmpv);
    this->downmix_combo          .observe(this->lmpv);
    this->volume_slider          .observe(this->lmpv);
    this->mute_checkbox          .observe(this->lmpv);
    this->audio_delay_slider     .observe(this->lmpv);
    this->sub_scale_slider       .observe(this->lmpv);
    this->sub_fps_combo          .observe(this->lmpv);
    this->sub_scale_slider       .observe(this->lmpv);
    this->sub_pos_slider         .observe(this->lmpv);
    this->embedded_fonts_checkbox.observe(this->lmpv);
    this->speed_slider           .observe(this->lmpv);
    this->cache_combo            .observe(this->lmpv);
    this->log_level_combo        .observe(this->lmpv);

    for (auto &prop: this->video_zoom_options)
        prop.observe(this->lmpv);

    for (auto &prop: this->video_color_options)
        prop.observe(this->lmpv);
}

PlayerMenu::~PlayerMenu() {
    this->lmpv.unobserve_property("track-list");
    this->lmpv.unobserve_property("playlist");
    this->lmpv.unobserve_property("file-format");
    this->lmpv.unobserve_property("video-codec");
    this->lmpv.unobserve_property("audio-codec");
    this->lmpv.unobserve_property("hwdec-current");
    this->lmpv.unobserve_property("hwdec-interop");
    this->lmpv.unobserve_property("avsync");
    this->lmpv.unobserve_property("frame-drop-count");
    this->lmpv.unobserve_property("decoder-frame-drop-count");
    this->lmpv.unobserve_property("video-bitrate");
    this->lmpv.unobserve_property("audio-bitrate");
    this->lmpv.unobserve_property("container-fps");
    this->lmpv.unobserve_property("estimated-vf-fps");
    this->lmpv.unobserve_property("video-unscaled");
    this->lmpv.unobserve_property("keepaspect");
    this->lmpv.unobserve_property("video-params");
    this->lmpv.unobserve_property("osd-dimensions");
    this->lmpv.unobserve_property("audio-params");
    this->lmpv.unobserve_property("profile-list");

    this->fbo_format_combo       .unobserve(this->lmpv);
    this->hdr_peak_checkbox      .unobserve(this->lmpv);
    this->deinterlace_checkbox   .unobserve(this->lmpv);
    this->aspect_ratio_combo     .unobserve(this->lmpv);
    this->rotation_combo         .unobserve(this->lmpv);
    this->downmix_combo          .unobserve(this->lmpv);
    this->volume_slider          .unobserve(this->lmpv);
    this->mute_checkbox          .unobserve(this->lmpv);
    this->audio_delay_slider     .unobserve(this->lmpv);
    this->sub_scale_slider       .unobserve(this->lmpv);
    this->sub_fps_combo          .unobserve(this->lmpv);
    this->sub_scale_slider       .unobserve(this->lmpv);
    this->sub_pos_slider         .unobserve(this->lmpv);
    this->embedded_fonts_checkbox.unobserve(this->lmpv);
    this->speed_slider           .unobserve(this->lmpv);
    this->cache_combo            .unobserve(this->lmpv);
    this->log_level_combo        .unobserve(this->lmpv);

    for (auto &prop: this->video_zoom_options)
        prop.unobserve(this->lmpv);

    for (auto &prop: this->video_color_options)
        prop.unobserve(this->lmpv);
}

bool PlayerMenu::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto now = std::chrono::system_clock::now();

    if ((padGetButtonsDown(&pad) & HidNpadButton_Y) && !ImGui::nx::isSwkbdVisible())
        this->is_visible ^= 1;

    if (now - this->last_stats_update > PlayerMenu::StatsRefreshInterval) {
        this->last_stats_update = now;
        this->lmpv.get_property_async("vo-passes", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
            auto *self   = static_cast<PlayerMenu *>(user);
            auto *node   = static_cast<mpv_node *>(prop->data);
            auto *passes = node->u.list;

            SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

            self->passes_info.clear();

            if (!passes)
                return;

            auto *fresh = LibmpvController::node_map_find<mpv_node_list *>(passes, "fresh");
            for (int i = 0; i < fresh->num; ++i) {
                auto *pass = fresh->values[i].u.list;
                auto *samples = LibmpvController::node_map_find<mpv_node_list *>(pass, "samples");

                auto &info = self->passes_info.emplace_back(
                    LibmpvController::node_map_find<char *>(pass, "desc"),
                    double(LibmpvController::node_map_find<std::int64_t>(pass, "avg"))  / 1.0e6,
                    double(LibmpvController::node_map_find<std::int64_t>(pass, "peak")) / 1.0e6,
                    double(LibmpvController::node_map_find<std::int64_t>(pass, "last")) / 1.0e6
                );

                info.samples.resize(samples->num);
                std::transform(samples->values, samples->values + samples->num, info.samples.begin(), [](auto &in) {
                    // implot renders doubles so we store that to avoid a conversion step
                    return double(in.u.int64) / 1.0e6;
                });
            }
        }, this);

        this->lmpv.get_property_async("demuxer-cache-state", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
            auto *self  = static_cast<PlayerMenu *>(user);
            auto *node  = static_cast<mpv_node *>(prop->data);
            auto *state = node->u.list;

            SW_SCOPEGUARD([&node] { mpv_free_node_contents(node); });

            if (!state)
                return;

            self->demuxer_cache_begin   = LibmpvController::node_map_find<double>(state, "reader-pts");
            self->demuxer_cache_end     = LibmpvController::node_map_find<double>(state, "cache-end");
            self->demuxer_cached_bytes  = LibmpvController::node_map_find<std::int64_t>(state, "total-bytes");
            self->demuxer_forward_bytes = LibmpvController::node_map_find<std::int64_t>(state, "fw-bytes");
            self->demuxer_cache_speed   = LibmpvController::node_map_find<std::int64_t>(state, "raw-input-rate");
        }, this);
    }

    if (this->is_filepicker(this->cur_subwindow))
        this->explorer.update_state(pad, touch);

    return false;
}

void PlayerMenu::render() {
    if (!this->is_visible)
        return;

    auto    &imio = ImGui::GetIO();
    auto &imstyle = ImGui::GetStyle();

    ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(PlayerMenu::MenuWidth, PlayerMenu::MenuHeight));
    ImGui::SetWindowPos (this->screen_rel_vec<ImVec2>(PlayerMenu::MenuPosX,  PlayerMenu::MenuPosY));
    ImGui::SetWindowFontScale(this->scale_factor());
    SW_SCOPEGUARD([] { ImGui::End(); });

    ImGui::BeginTabBar("##tabbar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton |
        ImGuiTabBarFlags_NoTabListScrollingButtons | ImGuiTabBarFlags_NoTooltip);
    SW_SCOPEGUARD([] { ImGui::EndTabBar(); });

    if (ImGui::BeginTabItem("Video", nullptr, ImGuiTabItemFlags_NoReorder)) {
        SW_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::SeparatorText("Track");
        this->video_tracklist.run(this->lmpv);

        ImGui::SeparatorText("Quality");

        if (ImGui::BeginCombo("Profile", "Choose profile")) {
            SW_SCOPEGUARD([] { ImGui::EndCombo(); });

            for (auto &profile: this->profile_list) {
                if (ImGui::Selectable(profile.c_str()))
                    this->lmpv.set_property_async("profile", profile.c_str());
            }
        }

        this->hdr_peak_checkbox.run(this->lmpv);

        if (ImGui::Button("Advanced##videoquality"))
            this->cur_subwindow = (this->cur_subwindow != SubwindowType::VideoQuality) ?
                SubwindowType::VideoQuality : SubwindowType::None;

        ImGui::SeparatorText("Window");

        constexpr static std::array scaling_opts = {
            "Stretch to fit"sv,
            ""sv,
            "Keep aspect ratio"sv,
            "Native"sv,
        };

        std::size_t scaling_opt = (this->keepaspect << 1) | (this->video_unscaled << 0);
        if (ImGui::BeginCombo("Scaling", scaling_opts[scaling_opt].data())) {
            SW_SCOPEGUARD([] { ImGui::EndCombo(); });

            for (std::size_t i = 0; i < scaling_opts.size(); ++i) {
                if (scaling_opts[i].empty())
                    continue;

                auto is_selected = scaling_opt == i;
                if (ImGui::Selectable(scaling_opts[i].data(), is_selected)) {
                    this->lmpv.set_property_async("video-unscaled", (i & (1 << 0)) ? "yes" : "no");
                    this->lmpv.set_property_async("keepaspect",     (i & (1 << 1)) ? "yes" : "no");
                }

                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
        }

        this->aspect_ratio_combo.run(this->lmpv);

        ImGui::SeparatorText("Other");

        if (ImGui::Button("Zoom/Position"))
            this->cur_subwindow = (this->cur_subwindow != SubwindowType::ZoomPos) ?
                SubwindowType::ZoomPos : SubwindowType::None;

        ImGui::SameLine();
        if (ImGui::Button("Color equalizer"))
            this->cur_subwindow = (this->cur_subwindow != SubwindowType::ColorEqualizer) ?
                SubwindowType::ColorEqualizer : SubwindowType::None;
    }

    if (ImGui::BeginTabItem("Audio", nullptr, ImGuiTabItemFlags_NoReorder)) {
        SW_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::SeparatorText("Track");
        this->audio_tracklist.run(this->lmpv);

        ImGui::SeparatorText("Channel mixing");
        this->downmix_combo.run(this->lmpv);

        ImGui::SeparatorText("Volume");
        this->volume_slider.run(this->lmpv, "Reset##volume");
        this->mute_checkbox.run(this->lmpv);

        ImGui::SeparatorText("Delay");
        this->audio_delay_slider.run(this->lmpv, "Reset##audiodelay");
    }

    if (ImGui::BeginTabItem("Subtitles", nullptr, ImGuiTabItemFlags_NoReorder)) {
        SW_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::SeparatorText("Track");
        this->sub_tracklist.run(this->lmpv);

        if (ImGui::Button("Load external file"))
            this->cur_subwindow = (this->cur_subwindow != SubwindowType::SubtitleFilepicker) ?
                SubwindowType::SubtitleFilepicker : SubwindowType::None;

        ImGui::SeparatorText("Delay");
        this->sub_delay_slider.run(this->lmpv, "Reset##subdelay");

        ImGui::SeparatorText("FPS");
        this->sub_fps_combo.run(this->lmpv);

        // TODO: subtitle speed?

        ImGui::SeparatorText("Size/position");
        this->sub_scale_slider.run(this->lmpv, "Reset##subscale");
        this->sub_pos_slider  .run(this->lmpv, "Reset##subpos");

        ImGui::SeparatorText("Style");
        this->embedded_fonts_checkbox.run(this->lmpv);
    }

    if (ImGui::BeginTabItem("Misc")) {
        SW_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::SeparatorText("Playlist");

        utils::StaticString32 id_buffer;
        auto make_id = [&id_buffer](std::size_t i, std::string_view s) {
            std::snprintf(id_buffer.data(), id_buffer.capacity(), "%s##%ld", s.data(), i);
            return id_buffer.data();
        };

        utils::StaticString32 cbuf1, cbuf2;
        std::size_t cur_playlist_selection = -1;
        if (ImGui::BeginListBox("##playlistlistbox", ImVec2(-1, 0))) {
            SW_SCOPEGUARD([] { ImGui::EndListBox(); });

            ImGui::SetWindowFontScale(0.8);
            SW_SCOPEGUARD([] { ImGui::SetWindowFontScale(1.0); });

            ImGuiListClipper clipper;
            clipper.Begin(this->playlist_info.size());

            while (clipper.Step()) {
                for (auto i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    auto &track = this->playlist_info[i];

                    if (track.id == this->playlist_selection_id)
                        cur_playlist_selection = i;

                    if (ImGui::Button(make_id(i, "\ue0f2"))) {
                        std::snprintf(cbuf1.data(), cbuf1.capacity(), "%ld", std::int64_t(i));
                        this->lmpv.command_async("playlist-remove", cbuf1.c_str());
                    }

                    ImGui::SameLine();
                    if (ImGui::RadioButton(make_id(i, "##"), track.id == this->playlist_selection_id))
                        this->playlist_selection_id = track.id;

                    ImGui::SameLine();
                    if (ImGui::Selectable(track.name.c_str(), track.playing))
                        lmpv.set_property_async("playlist-pos", std::int64_t(i));
                }
            }
        }

        if (ImGui::Button("\ue0f1"))
            this->cur_subwindow = (this->cur_subwindow != SubwindowType::PlaylistFilepicker) ?
                SubwindowType::PlaylistFilepicker : SubwindowType::None;

        ImGui::SameLine();
        if (ImGui::Button("\ue092") && (cur_playlist_selection != -1ull)) {
            std::snprintf(cbuf1.data(), cbuf1.capacity(), "%ld", cur_playlist_selection);
            std::snprintf(cbuf2.data(), cbuf2.capacity(), "%ld", cur_playlist_selection - 1);
            this->lmpv.command_async("playlist-move", cbuf1.c_str(), cbuf2.c_str());
        }

        ImGui::SameLine();
        if (ImGui::Button("\ue093") && (cur_playlist_selection != -1ull)) {
            std::snprintf(cbuf1.data(), cbuf1.capacity(), "%ld", cur_playlist_selection);
            std::snprintf(cbuf2.data(), cbuf2.capacity(), "%ld",
                (cur_playlist_selection == this->playlist_info.size() - 1) ? 0 : cur_playlist_selection + 2);
            this->lmpv.command_async("playlist-move", cbuf1.c_str(), cbuf2.c_str());
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear"))
            this->lmpv.command_async("playlist-clear");

        ImGui::SeparatorText("Speed");
        this->speed_slider.run(this->lmpv, "Reset##speed");

        ImGui::SeparatorText("Demuxer cache");
        this->cache_combo.run(this->lmpv);

        ImGui::SeparatorText("Log level");
        this->log_level_combo.run(this->lmpv);

        ImGui::SeparatorText("Other");
        ImGui::BeginGroup();
        ImGui::Checkbox("Fast presentation", &this->context.use_fast_presentation);

        ImGui::SameLine();
        ImGui::TextDisabled("\ue152"); // Question mark character in nintendo's extended font
        ImGui::EndGroup();

        if (ImGui::IsItemFocused() || ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20.0f);
            ImGui::TextUnformatted("Puts mpv in charge of frame presentation, "
                "resulting in more accurate timings and reduced GPU usage, "
                "however a black frame will be shown whenever the UI appears");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        ImGui::Checkbox("Disable screensaver",        &this->context.disable_screensaver);
        ImGui::Checkbox("Override screenshot button", &this->context.override_screenshot_button);
    }

    auto bullet_wrapped = [](std::string_view fmt, auto &&...args) {
        ImGui::Bullet();
        ImGui::TextWrapped(fmt.data(), std::forward<decltype(args)>(args)...);
    };

    if (ImGui::BeginTabItem("Stats")) {
        SW_SCOPEGUARD([] { ImGui::EndTabItem(); });

        ImGui::BeginTabBar("##statstabbar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton |
            ImGuiTabBarFlags_NoTabListScrollingButtons | ImGuiTabBarFlags_NoTooltip);
        SW_SCOPEGUARD([] { ImGui::EndTabBar(); });

        if (ImGui::BeginTabItem("Info")) {
            SW_SCOPEGUARD([] { ImGui::EndTabItem(); });

            ImGui::SetWindowFontScale(0.68 * this->scale_factor());
            SW_SCOPEGUARD([&] { ImGui::SetWindowFontScale(this->scale_factor()); });

            ImGui::SeparatorText("Source");
            bullet_wrapped("Format: %s", this->file_format);

            ImGui::SeparatorText("Video");
            bullet_wrapped("Codec: %s", this->video_codec);
            if (this->hwdec_current)
                bullet_wrapped("hwdec: %s", this->hwdec_current);
            bullet_wrapped("Framerate: %.3fHz (specified) %.3fHz (estimated)",
                this->container_specified_fps, this->container_estimated_fps);
            bullet_wrapped("A/V desync: %+.3fs", this->avsync);
            bullet_wrapped("Dropped: %ld (VO) %ld (decoder)", this->dropped_vo_frames, this->dropped_dec_frames);
            bullet_wrapped("Size: %dx%d, scaled: %dx%d", this->video_width, this->video_height,
                this->video_width_scaled, this->video_height_scaled);
            if (!this->video_hw_pixfmt.empty())
                bullet_wrapped("Pixel format: %s [%s]",
                    this->video_pixfmt.c_str(), this->video_hw_pixfmt.c_str());
            else
                bullet_wrapped("Pixel format: %s", this->video_pixfmt.c_str());
            bullet_wrapped("Colorspace: %s, range: %s, gamma: %s", this->video_colorspace.c_str(),
                this->video_color_range.c_str(), this->video_gamma.c_str());
            bullet_wrapped("Bitrate: %.2fkbps\n", float(this->video_bitrate) / 1000.0f);

            ImGui::SeparatorText("Audio");
            bullet_wrapped("Codec: %s", this->audio_codec);
            bullet_wrapped("Layout: %s (%d channels)", this->audio_layout.c_str(), this->audio_num_channels);
            bullet_wrapped("Format: %s", this->audio_format.c_str());
            bullet_wrapped("Samplerate: %dHz", this->audio_samplerate);
            bullet_wrapped("Bitrate: %.2fkbps\n", float(this->audio_bitrate) / 1000.0f);

            ImGui::SeparatorText("Cache");
            bullet_wrapped("Packet queue: %02u:%02u:%02u\u2012%02u:%02u:%02u (%02u:%02u:%02u)",
                FORMAT_TIME(std::uint32_t(this->demuxer_cache_begin)), FORMAT_TIME(std::uint32_t(this->demuxer_cache_end)),
                FORMAT_TIME(std::uint32_t(this->demuxer_cache_end - this->demuxer_cache_begin)));
            bullet_wrapped("RAM used: %.02fMiB (%.2fMiB forward)", double(this->demuxer_cached_bytes)/0x100000,
                double(this->demuxer_forward_bytes)/0x100000);
            bullet_wrapped("Speed: %.2fMiB/s", this->demuxer_cache_speed/0x100000);

            ImGui::SeparatorText("Interface");
            bullet_wrapped("FPS: %.2fHz, frame time %.2fms", imio.Framerate, imio.DeltaTime * 1000.0f);
            bullet_wrapped("Vertices: %d", imio.MetricsRenderVertices);
            bullet_wrapped("Indices: %d", imio.MetricsRenderIndices);
        }

        if (ImGui::BeginTabItem("Passes")) {
            SW_SCOPEGUARD([] { ImGui::EndTabItem(); });

            ImGui::RadioButton("Graphs",    &this->perf_plot_is_pie, false); ImGui::SameLine();
            ImGui::RadioButton("Pie chart", &this->perf_plot_is_pie, true);

            if (this->perf_plot_is_pie) {
                ImGui::RadioButton("Average", &this->perf_plot_pie_type, 0); ImGui::SameLine();
                ImGui::RadioButton("Peak",    &this->perf_plot_pie_type, 1); ImGui::SameLine();
                ImGui::RadioButton("Last",    &this->perf_plot_pie_type, 2);
            }

            ImGui::SetWindowFontScale(0.5 * this->scale_factor());
            SW_SCOPEGUARD([&] { ImGui::SetWindowFontScale(this->scale_factor()); });

            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_WindowBg));
            SW_SCOPEGUARD([] { ImGui::PopStyleColor(); });

            auto plot_flags = ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs |
                ImPlotFlags_NoFrame | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                (this->perf_plot_is_pie ? ImPlotFlags_Equal : 0) /* |
                (this->passes_info.size() > 20 ? ImPlotFlags_NoLegend : 0) */;

            ImPlot::PushColormap(ImPlotColormap_Dark);
            SW_SCOPEGUARD([] { ImPlot::PopColormap(); });

            if (ImPlot::BeginPlot("Shader passes", ImVec2(-1, -1), plot_flags)) {
                SW_SCOPEGUARD([] { ImPlot::EndPlot(); });

                auto axes_flags = ImPlotAxisFlags_AutoFit |
                    (this->perf_plot_is_pie ? ImPlotAxisFlags_NoDecorations : 0);

                ImPlot::SetupAxes("", "ms", axes_flags, axes_flags);
                ImPlot::SetupLegend(ImPlotLocation_South, ImPlotLegendFlags_Outside);

                if (!this->perf_plot_is_pie) {
                    for (auto &stats: this->passes_info)
                        ImPlot::PlotLine(stats.desc.c_str(), stats.samples.data(), stats.samples.size());
                } else {
                    auto names  = std::vector<const char *>(this->passes_info.size());
                    auto values = std::vector<double>(this->passes_info.size());

                    std::transform(this->passes_info.begin(), this->passes_info.end(), names.begin(),
                        [](auto &&pass) { return pass.desc.c_str(); });

                    std::transform(this->passes_info.begin(), this->passes_info.end(), values.begin(),
                        [this](auto &&pass) { return *(&pass.average + this->perf_plot_pie_type); });

                    ImPlot::PlotPieChart(names.data(), values.data(), this->passes_info.size(),
                        0.5, 0.5, 0.4, "%.2fms", 0.0, ImPlotPieChartFlags_Normalize);
                }
            }
        }
    }

    if (this->cur_subwindow != SubwindowType::None) {
        std::string_view title;
        switch (this->cur_subwindow) {
            case SubwindowType::VideoQuality:
                title = "Advanced video quality##window";
                break;
            case SubwindowType::ZoomPos:
                title = "Zoom##window";
                break;
            case SubwindowType::ColorEqualizer:
                title = "Color equalizer##window";
                break;
            case SubwindowType::ShaderFilepicker:
                title = "Custom shader##window";
                break;
            case SubwindowType::SubtitleFilepicker:
                title = "External subtitles##window";
                break;
            case SubwindowType::PlaylistFilepicker:
                title = "Playlist##window";
                break;
            case SubwindowType::None:
            default:
                break;
        }

        ImGui::Begin(title.data(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetWindowFontScale(this->scale_factor());
        SW_SCOPEGUARD([] { ImGui::End(); });

        switch (this->cur_subwindow) {
            case SubwindowType::ShaderFilepicker:
            case SubwindowType::SubtitleFilepicker:
            case SubwindowType::PlaylistFilepicker:
                ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(PlayerMenu::FilepickerWidth, PlayerMenu::FilepickerHeight));
                ImGui::SetWindowPos (this->screen_rel_vec<ImVec2>(PlayerMenu::FilepickerPosX,  PlayerMenu::FilepickerPosY));
                break;
            case SubwindowType::VideoQuality:
                ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(PlayerMenu::SubMenuWidth, PlayerMenu::VideoSubMenuHeight));
                ImGui::SetWindowPos (this->screen_rel_vec<ImVec2>(PlayerMenu::SubMenuPosX,  PlayerMenu::SubMenuPosY));
                break;
            default:
                ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(PlayerMenu::SubMenuWidth, PlayerMenu::SubMenuHeight));
                ImGui::SetWindowPos (this->screen_rel_vec<ImVec2>(PlayerMenu::SubMenuPosX,  PlayerMenu::SubMenuPosY));
                break;
        }

        auto run_option_group = [this, &imstyle](auto &options) {
            for (auto &prop: options)
                prop.run(this->lmpv);

            if (ImGui::Button("Reset")) {
                for (auto &prop: options)
                    prop.reset(this->lmpv);
            }
        };

        auto run_reset_button = [this, &imstyle]() {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y -
                ImGui::GetFontSize() - imstyle.ItemSpacing.y);
            if (ImGui::Button("Return"))
                this->cur_subwindow = SubwindowType::None;
        };

        auto run_filepicker = [this, &imstyle](SubwindowType type) {
            this->explorer.render();

            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(imstyle.ItemSpacing.x, ImGui::GetCursorPosY() - imstyle.ItemSpacing.y));
            if (ImGui::Button("Return"))
                this->cur_subwindow = (type == SubwindowType::ShaderFilepicker) ?
                    SubwindowType::VideoQuality : SubwindowType::None;
        };

        static constinit std::array tap_options = {
            std::pair{"5-tap"sv,  "5tap"sv},
            std::pair{"10-tap"sv, "10tap"sv},
        };

        auto run_vic_spatialfilter = [&](std::string_view display_name, std::string_view filter_name,
                std::string_view label, std::string_view strength_param,
                bool &has_filter, float &strength, int &dimensions)
        {
            utils::StaticString32 id_buffer;
            auto make_id = [&id_buffer](std::string_view l, std::string_view s) {
                std::snprintf(id_buffer.data(), id_buffer.capacity(), "%s##%s", s.data(), l.data());
                return id_buffer.data();
            };

            if (ImGui::Checkbox(make_id(label, display_name), &has_filter)) {
                if (has_filter) {
                    utils::StaticString<128> cmd;
                    std::snprintf(cmd.data(), cmd.capacity(),
                        "@%s:lavfi=[%s=%s=%f:dimensions=%s]",
                        label.data(),
                        filter_name.data(), strength_param.data(),
                        strength, tap_options[dimensions].second.data());

                    this->lmpv.command_async("vf", "add", cmd.c_str());
                } else {
                    utils::StaticString32 cmd;
                    std::snprintf(cmd.data(), cmd.capacity(), "@%s", label.data());
                    this->lmpv.command_async("vf", "remove", cmd.data());
                }
            }

            ImGui::Indent();
            SW_SCOPEGUARD([] { ImGui::Unindent(); });

            if (ImGui::SliderFloat(make_id(label, "Strength"), &strength, 0.0f, 1.0f, "%.2f") && has_filter) {
                utils::StaticString32 cmd;
                std::snprintf(cmd.data(), cmd.capacity(), "%f", strength);
                this->lmpv.command_async("vf-command", label.data(), strength_param.data(), cmd.c_str());
            }

            if (ImGui::BeginCombo(make_id(label, "Area"), tap_options[dimensions].first.data())) {
                SW_SCOPEGUARD([] { ImGui::EndCombo(); });

                for (std::size_t i = 0; i < tap_options.size(); ++i) {
                    bool is_selected = std::size_t(dimensions) == i;
                    if (ImGui::Selectable(tap_options[i].first.data(), is_selected)) {
                        dimensions = i;

                        this->lmpv.command_async("vf-command", label.data(), "dimensions",
                            tap_options[i].second.data());
                    }

                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
            }
        };

        switch (this->cur_subwindow) {
            case SubwindowType::VideoQuality:
                if (ImGui::Button("Load external shader"))
                    this->cur_subwindow = (this->cur_subwindow != SubwindowType::ShaderFilepicker) ?
                        SubwindowType::ShaderFilepicker : SubwindowType::None;

                this->fbo_format_combo    .run(this->lmpv);

                this->use_hwdec_checkbox  .run(this->lmpv, +[]([[maybe_unused]] LibmpvController &lmpv, bool val) {
                    return val ? "auto" : "no";
                });

                this->deinterlace_checkbox.run(this->lmpv);

                ImGui::SeparatorText("Hardware filters");

                run_vic_spatialfilter("Sharpness", "sharpness_nvtegra", "vicsharp", "sharpness",
                    has_sharpness_filter, sharpness_value, sharpness_dimensions);
                run_vic_spatialfilter("Denoise",   "denoise_nvtegra",   "vicnoise", "denoise",
                    has_denoise_filter,   denoise_value,   denoise_dimensions);

                constexpr static auto hw_deint_filter_name = "vicdeint"sv;

                static constinit std::array deint_mode_options = {
                    std::pair{"Weave"sv, "weave"sv},
                    std::pair{"Bob"sv,   "bob"sv},
                };

                if (ImGui::Checkbox("Deinterlacing", &has_hw_deinterlace)) {
                    if (has_hw_deinterlace) {
                        utils::StaticString<128> cmd;
                        std::snprintf(cmd.data(), cmd.capacity(),
                            "@%s:lavfi=[deinterlace_nvtegra=mode=%s]",
                            hw_deint_filter_name.data(),
                            deint_mode_options[this->hw_deinterlace_mode].second.data());

                        this->lmpv.command_async("vf", "add", cmd.c_str());
                    } else {
                        utils::StaticString32 cmd;
                        std::snprintf(cmd.data(), cmd.capacity(), "@%s", hw_deint_filter_name.data());
                        this->lmpv.command_async("vf", "remove", cmd.data());
                    }
                }

                {
                    ImGui::Indent();
                    SW_SCOPEGUARD([] { ImGui::Unindent(); });

                    if (ImGui::BeginCombo("Mode", deint_mode_options[this->hw_deinterlace_mode].first.data())) {
                        SW_SCOPEGUARD([] { ImGui::EndCombo(); });

                        for (std::size_t i = 0; i < deint_mode_options.size(); ++i) {
                            bool is_selected = std::size_t(this->hw_deinterlace_mode) == i;
                            if (ImGui::Selectable(deint_mode_options[i].first.data(), is_selected)) {
                                this->hw_deinterlace_mode = i;

                                this->lmpv.command_async("vf-command", hw_deint_filter_name.data(), "mode",
                                    deint_mode_options[this->hw_deinterlace_mode].second.data());
                            }

                            if (is_selected)
                                ImGui::SetItemDefaultFocus();
                        };
                    }
                }

                run_reset_button();
                break;
            case SubwindowType::ZoomPos:
                run_option_group(this->video_zoom_options);

                ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, 3.0f);
                this->rotation_combo.run(this->lmpv);

                run_reset_button();
                break;
            case SubwindowType::ColorEqualizer:
                run_option_group(this->video_color_options);
                run_reset_button();
                break;
            case SubwindowType::ShaderFilepicker:
            case SubwindowType::SubtitleFilepicker:
            case SubwindowType::PlaylistFilepicker:
                run_filepicker(this->cur_subwindow);
                break;
            case SubwindowType::None:
            default:
                break;
        }
    }

    if (!this->explorer.selection.empty()) {
        switch (this->cur_subwindow) {
            case SubwindowType::ShaderFilepicker:
                this->lmpv.command_async("change-list", "glsl-shaders", "append", this->explorer.selection.c_str());
                this->cur_subwindow = SubwindowType::VideoQuality;
                break;
            case SubwindowType::SubtitleFilepicker:
                this->lmpv.command_async("sub-add", this->explorer.selection.c_str());
                this->cur_subwindow = SubwindowType::None;
                break;
            case SubwindowType::PlaylistFilepicker:
                this->lmpv.command_async("loadfile", this->explorer.selection.c_str(), "append");
                this->cur_subwindow = SubwindowType::None;
                break;
            default:
                break;
        }

        this->explorer.selection.clear();
    }
}

Console::Console(Renderer &renderer, LibmpvController &lmpv): Widget(renderer), lmpv(lmpv) {
    Console::s_this = this;

    this->lmpv.set_log_callback(+[](void *user, mpv_event_log_message *msg) {
#ifdef DEBUG
        std::printf("[%s]: %s", msg->prefix, msg->text);
#endif

        Console *self = static_cast<Console *>(user);

        if (self->is_frozen)
            return;

        auto str = std::format("[{}] {}", msg->prefix, msg->text);

        self->logs.emplace_back(msg->log_level, std::move(str));

        if (self->logs.size() > Console::ConsoleMaxLogs)
            self->logs.pop_front();
    }, this);

    this->input_text.reserve(0x1000);
    std::fill(this->input_text.begin(), this->input_text.end(), '\0');

    swkbdInlineMakeAppearArg(&this->appear_args, SwkbdType_Normal);
    this->appear_args.dicFlag          = 0;
    this->appear_args.returnButtonFlag = 0;

    swkbdInlineSetKeytopBgAlpha(ImGui::nx::getSwkbd(), 0.75f);
    swkbdInlineSetFooterBgAlpha(ImGui::nx::getSwkbd(), 0.75f);

    swkbdInlineSetChangedStringCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdChangedStringArg *arg) {
        if (arg->stringLen <= s_this->input_text.capacity())
            s_this->input_text = str;

        s_this->cursor_pos         = arg->cursorPos;
        s_this->want_cursor_update = true;
    });

    swkbdInlineSetMovedCursorCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdMovedCursorArg *arg) {
        if (arg->cursorPos == s_this->cursor_pos)
            return;

        s_this->cursor_pos         = arg->cursorPos;
        s_this->want_cursor_update = true;
    });

    swkbdInlineSetDecidedEnterCallback(ImGui::nx::getSwkbd(), +[](const char *str, SwkbdDecidedEnterArg *arg) {
        s_this->cmd_history.emplace_back(str);
        if (s_this->cmd_history.size() > Console::ConsoleMaxHistory)
            s_this->cmd_history.pop_front();

        s_this->cmd_history_pos = s_this->cmd_history.cend();

        s_this->input_text         = "";
        s_this->cursor_pos         = 0;
        s_this->want_cursor_update = true;

        swkbdInlineSetInputText(ImGui::nx::getSwkbd(), s_this->input_text.c_str());
        swkbdInlineSetCursorPos(ImGui::nx::getSwkbd(), s_this->cursor_pos);

        // Exit input box
        ImGui::GetCurrentContext()->ActiveId = 0;

        mpv_command_string(s_this->lmpv.get_handle(), str);
    });

    swkbdInlineSetDecidedCancelCallback(ImGui::nx::getSwkbd(), +[]() {
        // Exit input box
        ImGui::GetCurrentContext()->ActiveId = 0;
    });
}

Console::~Console() {
    swkbdInlineSetChangedStringCallback(ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetMovedCursorCallback  (ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetDecidedEnterCallback (ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetDecidedCancelCallback(ImGui::nx::getSwkbd(), nullptr);
    swkbdInlineSetInputText(ImGui::nx::getSwkbd(), "");
    swkbdInlineSetCursorPos(ImGui::nx::getSwkbd(), 0);
}

bool Console::update_state(PadState &pad, HidTouchScreenState &touch) {
    if ((padGetButtonsDown(&pad) & HidNpadButton_Minus) && !ImGui::nx::isSwkbdVisible())
        this->is_visible ^= 1;

    return false;
}

void Console::set_text(const std::string_view text) {
    // Flush pending requests
    swkbdInlineUpdate(ImGui::nx::getSwkbd(), nullptr);

    this->input_text = text;

    swkbdInlineSetInputText(ImGui::nx::getSwkbd(), text.data());
    swkbdInlineSetCursorPos(ImGui::nx::getSwkbd(), text.length());
    swkbdInlineUpdate(ImGui::nx::getSwkbd(), nullptr);
}

void Console::render() {
    if (!this->is_visible) {
        if (ImGui::nx::isSwkbdVisible())
            ImGui::nx::hideSwkbd();
        return;
    }

    ImGui::Begin("Console", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowSize(this->screen_rel_vec<ImVec2>(Console::ConsoleWidth, Console::ConsoleHeight));
    ImGui::SetWindowPos (this->screen_rel_vec<ImVec2>(Console::ConsolePosX,  Console::ConsolePosY));
    ImGui::SetWindowFontScale(this->scale_factor());
    SW_SCOPEGUARD([] { ImGui::End(); });

    ImGui::SetWindowFontScale(0.8 * this->scale_factor());
    SW_SCOPEGUARD([&] { ImGui::SetWindowFontScale(this->scale_factor()); });

    ImGui::PushItemWidth(-1);
    ImGui::InputText("##input", this->input_text.data(), this->input_text.capacity(),
        ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_CallbackAlways, +[](ImGuiInputTextCallbackData *data) -> int {
            auto *self = static_cast<Console *>(data->UserData);

            if (self->want_cursor_update) {
                data->CursorPos          = self->cursor_pos;
                self->want_cursor_update = false;
            }

            if (data->CursorPos != self->cursor_pos)
                swkbdInlineSetCursorPos(ImGui::nx::getSwkbd(), data->CursorPos);

            self->cursor_pos = data->CursorPos;
            data->ClearSelection();

            return 0;
    }, this);
    ImGui::PopItemWidth();

    if (ImGui::IsItemActive()) {
        if (!ImGui::nx::isSwkbdVisible())
            ImGui::nx::showSwkbd(&this->appear_args);
    } else {
        if (ImGui::nx::isSwkbdVisible())
            ImGui::nx::hideSwkbd();
    }

    if (ImGui::Button("\ue092") && this->cmd_history.size()) {
        if (this->cmd_history_pos != this->cmd_history.begin())
            this->set_text(*--this->cmd_history_pos);
    }

    ImGui::SameLine();
    if (ImGui::Button("\ue093") && this->cmd_history.size()) {
        if (this->cmd_history_pos != this->cmd_history.end() &&
                ++this->cmd_history_pos != this->cmd_history.end())
            this->set_text(*this->cmd_history_pos);
        else
            this->set_text("");
    }

    ImGui::SameLine(0.0f, this->screen_rel_width(0.23f));
    if (ImGui::Button("Clear"))
        this->logs.clear();

    ImGui::SameLine();
    ImGui::Selectable("Freeze", &this->is_frozen, 0, ImVec2(this->screen_rel_width(0.051f), 0));

    {
        ImGui::SetWindowFontScale(0.5 * this->scale_factor());
        SW_SCOPEGUARD([&] { ImGui::SetWindowFontScale(this->scale_factor()); });

        ImGui::BeginChild("##logregion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        SW_SCOPEGUARD([&] { ImGui::EndChild(); });

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
        SW_SCOPEGUARD([&] { ImGui::PopStyleVar(); });

        auto theme = ImGui::nx::getCurrentTheme();

        for (auto &log: this->logs)
            ImGui::TextColored(ImColor(this->map_log_level_color(log.level, theme == ColorSetId_Dark)),
                log.message.c_str());

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
}

} // namespace sw::ui
