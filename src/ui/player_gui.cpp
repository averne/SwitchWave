#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include "../imgui_impl_hos/imgui_deko3d.h"

#include "../utils.hpp"

#include "player_gui.hpp"

namespace ampnx {

#define FORMAT_TIME(s) (s)/60/60%99, (s)/60%60, (s)%60

bool PlayerGui::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto down = padGetButtonsDown(&pad);

    if (down & HidNpadButton_B)
        return false;

    this->seek_bar.update_state(pad, touch);

    return true;
}

void PlayerGui::render() {
    this->seek_bar.render();
}

SeekBar::SeekBar(Renderer &renderer, LibmpvController &lmpv): Widget(renderer), lmpv(lmpv) {
    this->play_texture      = this->renderer.load_texture("romfs:/textures/play-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->pause_texture     = this->renderer.load_texture("romfs:/textures/pause-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->previous_texture  = this->renderer.load_texture("romfs:/textures/previous-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->next_texture      = this->renderer.load_texture("romfs:/textures/next-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);

    this->lmpv.observe_property("pause",       &this->is_paused);
    this->lmpv.observe_property("time-pos",    &this->time_pos);
    this->lmpv.observe_property("duration",    &this->duration);
    this->lmpv.observe_property("percent-pos", &this->percent_pos);

    this->lmpv.observe_property("chapter-list", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self     = static_cast<SeekBar  *>(user);
        auto *node     = static_cast<mpv_node *>(prop->data);
        auto *chapters = node->u.list;

        self->chapters.resize(chapters->num);

        for (int i = 0; i < chapters->num; ++i) {
            auto *chapter = chapters->values[i].u.list;

            self->chapters[i] = {
                .title = LibmpvController::node_map_find<char *>(chapter, "title"),
                .time  = LibmpvController::node_map_find<double>(chapter, "time" ),
            };
        }

        mpv_free_node_contents(node);
    }, this);

    this->lmpv.observe_property("demuxer-cache-state", MPV_FORMAT_NODE, nullptr, +[](void *user, mpv_event_property *prop) {
        auto *self        = static_cast<SeekBar  *>(user);
        auto *node        = static_cast<mpv_node *>(prop->data);
        auto *cache_state = node->u.list;

        auto *ranges = LibmpvController::node_map_find<mpv_node_list *>(cache_state, "seekable-ranges");

        self->seekable_ranges.resize(ranges->num);

        for (int i = 0; i < ranges->num; ++i) {
            auto *range = ranges->values[i].u.list;

            self->seekable_ranges[i] = {
                .start = LibmpvController::node_map_find<double>(range, "start"),
                .end   = LibmpvController::node_map_find<double>(range, "end"  ),
            };
        }

        mpv_free_node_contents(node);
    }, this);
}

SeekBar::~SeekBar() {
    this->lmpv.unobserve_property("pause");
    this->lmpv.unobserve_property("time-pos");
    this->lmpv.unobserve_property("duration");
    this->lmpv.unobserve_property("percent-pos");
    this->lmpv.unobserve_property("chapter-list");
    this->lmpv.unobserve_property("demuxer-cache-state");
}

bool SeekBar::update_state(PadState &pad, HidTouchScreenState &touch) {
    auto down = padGetButtonsDown(&pad);

    if (!this->visible && down & HidNpadButton_A)
        lmpv.command_async("cycle", "pause");

    if (!this->visible && down & HidNpadButton_Y)
        lmpv.command_async("screenshot-to-file", "screenshot.png", "subtitles");

    constexpr auto pop_seekbar_mask = HidNpadButton_A | HidNpadButton_Left | HidNpadButton_Up |
        HidNpadButton_Right | HidNpadButton_Down;
    if (touch.count || (!this->visible && down & pop_seekbar_mask))
        this->begin_visible(true);

    auto now = std::chrono::system_clock::now();
    if (down) {
        this->visible_start = now;
    } else if (visible) {
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
            this->visible = false, this->fadeio_alpha = 0.0f;
    }

    if (down & (HidNpadButton_R | HidNpadButton_L))
        lmpv.seek((down & HidNpadButton_R) ? 5 : -5);

    if (down & (HidNpadButton_ZR | HidNpadButton_ZL))
        lmpv.seek((down & HidNpadButton_ZR) ? 60 : -60);

    return false;
}

void SeekBar::render() {
    if (!this->visible)
        return;

    auto &style =  ImGui::GetStyle();
    auto &imctx = *ImGui::GetCurrentContext();

    imctx.NavDisableHighlight = false;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,            this->fadeio_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    AMPNX_SCOPEGUARD([] { ImGui::PopStyleVar(); });
    AMPNX_SCOPEGUARD([] { ImGui::PopStyleVar(); });

    ImGui::SetNextWindowFocus();
    ImGui::Begin("##seekbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowSize(Widget::screen_rel_vec<ImVec2>(SeekBar::BarWidth, SeekBar::BarHeight));
    ImGui::SetWindowPos (Widget::screen_rel_vec<ImVec2>((1.0 - SeekBar::BarWidth) / 2.0, 1.0 - SeekBar::BarHeight));
    ImGui::SetWindowFontScale(float(this->renderer.image_height) / 720.0f);

    auto window_height = ImGui::GetWindowHeight();
    auto img_size = window_height - 2 * (style.WindowPadding.y + style.FramePadding.y);
    auto img_handle = ImGui::deko3d::makeTextureID(
        (this->is_paused ? this->play_texture : this->pause_texture).handle, true);

    // Workaround, due to how ImGui calculates ImageButton ids internally
    // the button gets deselected when changing the texture
    auto ImageButtonWidthId = [style](ImGuiID id, ImTextureID user_texture_id, const ImVec2& size,
            const ImVec2& uv0=ImVec2(0, 0), const ImVec2& uv1=ImVec2(1,1), int frame_padding=-1,
            const ImVec4& bg_col=ImVec4(0,0,0,0), const ImVec4& tint_col=ImVec4(1,1,1,1)) {
        const ImVec2 padding=(frame_padding >= 0) ? ImVec2((float)frame_padding, (float)frame_padding) : style.FramePadding;
        return ImGui::ImageButtonEx(id, user_texture_id, size, uv0, uv1, padding, bg_col, tint_col);
    };

    // Play/pause/skip buttons
    auto playpause_id = ImGui::GetID("playpause");
    if (ImageButtonWidthId(playpause_id, img_handle, ImVec2(img_size, img_size)))
        lmpv.command_async("cycle", "pause");
    ImGui::SetItemDefaultFocus();
    if (this->is_appearing)
        ImGui::SetNavID(playpause_id, imctx.NavLayer, 0, ImRect());

    ImGui::SameLine();
    ImGui::ImageButton(ImGui::deko3d::makeTextureID(this->previous_texture.handle, true),
        ImVec2{img_size, img_size});

    ImGui::SameLine();
    ImGui::ImageButton(ImGui::deko3d::makeTextureID(this->next_texture.handle,     true),
        ImVec2{img_size, img_size});

    auto text_yoffset = (window_height - ImGui::GetFontSize()) / 2.0f;

    // Current timestamp
    ImGui::SameLine(); ImGui::SetCursorPosY(text_yoffset);
    ImGui::Text("%02u:%02u:%02u", FORMAT_TIME(std::uint32_t(this->time_pos)));

    // Seek bar
    ImGui::SameLine(); ImGui::SetCursorPosY(SeekBar::SeekBarContourPx / 2);
    auto cursor = ImGui::GetCursorScreenPos();
    auto padding = ImVec2(Widget::screen_rel_height(SeekBar::SeekBarPadding),
        Widget::screen_rel_height(SeekBar::SeekBarPadding));
    auto bb          = ImRect(cursor, cursor + Widget::screen_rel_vec<ImVec2>(0.7, SeekBar::SeekBarHeight));
    auto interior_bb = ImRect(bb.Min + padding, bb.Max - padding);

    ImGui::ItemSize(bb);
    ImGui::ItemAdd(bb, ImGui::GetID("seekbar"), nullptr, ImGuiItemFlags_Disabled);

    auto *list = ImGui::GetWindowDrawList();
    list->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_Button), 0, 0, SeekBar::SeekBarContourPx);

    list->AddRectFilled(interior_bb.Min,
        ImVec2{interior_bb.Min.x + interior_bb.GetWidth() * float(this->percent_pos) / 100.0f, interior_bb.Max.y},
        ImGui::GetColorU32(ImGuiCol_ButtonActive));

    auto to_seekbar_pos = [this, &interior_bb](double timestamp) {
        return std::round(interior_bb.Min.x +
            interior_bb.GetWidth() * float(timestamp) / float(this->duration));
    };

    for (auto &chapter: this->chapters) {
        // Skip the first chapter
        if (chapter.time == 0.0)
            continue;

        auto pos_x = to_seekbar_pos(chapter.time);
        list->AddLine(ImVec2{pos_x, interior_bb.Min.y},
            ImVec2{pos_x, interior_bb.Max.y},
            ImGui::GetColorU32({1.0, 1.0, 1.0, 1.0}), SeekBar::SeekBarLinesWidthPx);
    }

    auto pos_y = interior_bb.Max.y - SeekBar::SeekBarLinesWidthPx / 2;
    for (auto &range: this->seekable_ranges) {
        list->AddLine(ImVec2{to_seekbar_pos(range.start), pos_y},
            ImVec2{to_seekbar_pos(range.end), pos_y},
            ImGui::GetColorU32({1.0, 1.0, 1.0, 1.0}), SeekBar::SeekBarLinesWidthPx);
    }

    // Total duration
    ImGui::SameLine(); ImGui::SetCursorPosY(text_yoffset);
    ImGui::Text("%02u:%02u:%02u", FORMAT_TIME(std::uint32_t(this->duration)));

    ImGui::End();
}


PlayerMenu::PlayerMenu(Renderer &renderer, LibmpvController &lmpv): Widget(renderer), lmpv(lmpv) {

}

PlayerMenu::~PlayerMenu() {

}

bool PlayerMenu::update_state(PadState &pad, HidTouchScreenState &touch) {

}

void PlayerMenu::render() {

}

} // namespace ampnx


// {
//     auto *reply = static_cast<mpv_event_property *>(mp_event->data);
//     auto *node = static_cast<mpv_node *>(reply->data);

//     if (!mp_event->error && vo_passes_property == reply->name && reply->format == MPV_FORMAT_NODE && reply->data) {
//         auto *fresh = node_map_find(node->u.list, "fresh").u.list;
//         passes_stats.resize(fresh->num);

//         for (int i = 0; i < fresh->num; ++i) {
//             mpv_node_list *pass = fresh->values[i].u.list;
//             auto &stats = passes_stats[i];
//             stats.desc = node_map_find(pass, "desc").u.string;

//             auto *samples = node_map_find(pass, "samples").u.list;
//             for (int j = 0; j < samples->num; ++j)
//                 stats.samples[j] = samples->values[j].u.int64/1000;
//             stats.num_samples = samples->num;
//         }
//     }
//     mpv_free_node_contents(node);
//     if (enable_stats)
//         lmpv.get_property_async<mpv_node>(vo_passes_property.data());
        // ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_Once);

// if (ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
//     ImGui::Text("Fps: %.1f, vtx: %d, idx: %d", io.Framerate, io.MetricsRenderVertices, io.MetricsRenderIndices);

//     bool prev_enable_stats = enable_stats;
//     if (ImGui::Checkbox("Enable stats", &enable_stats) && !prev_enable_stats)
//         lmpv.get_property_async<mpv_node>(vo_passes_property.data());

//     auto plot_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs |
//         ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_AntiAliased;
//     if (enable_stats && ImPlot::BeginPlot("fresh", ImVec2(600, 500), plot_flags)) {
//         ImPlot::SetupAxes("", "µs", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
//         ImPlot::SetupLegend(ImPlotLocation_South, ImPlotLegendFlags_Outside);

//         for (auto &stats: passes_stats)
//             ImPlot::PlotLine(stats.desc.c_str(), stats.samples.data(), stats.num_samples);

//         ImPlot::EndPlot();
//     }
// }
// ImGui::End();