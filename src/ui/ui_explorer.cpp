#include <dirent.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_nx.h>
#include <imgui_deko3d.h>

#include "utils.hpp"

#include "ui/ui_explorer.hpp"

namespace sw::ui {

namespace {

extern "C" {
    u32 __nx_fsdev_direntry_cache_size = 64;
}

std::string_view utf8_skip_from_end(std::string_view sv, int skip) {
    auto *data = sv.data() + sv.length();
    for (int i = 0; (i < skip) && (data > sv.data()); ++i)
        while ((*--data & 0xc0) == 0x80);
    return sv.substr(uintptr_t(data - sv.data()));
}

} // namespace

Explorer::Explorer(Renderer &renderer, Context &context): Widget(renderer), context(context) {
    this->path = !this->context.cur_path.empty() ? this->context.cur_path : "sdmc:/";

    this->file_texture    = this->renderer.load_texture("romfs:/textures/file-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->folder_texture  = this->renderer.load_texture("romfs:/textures/folder-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->recent_texture  = this->renderer.load_texture("romfs:/textures/recent-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->sd_texture      = this->renderer.load_texture("romfs:/textures/sd-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->usb_texture     = this->renderer.load_texture("romfs:/textures/usb-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
    this->network_texture = this->renderer.load_texture("romfs:/textures/network-64*64-bc4.bc",
        64, 64, DkImageFormat_R_BC4_Unorm, DkImageFlags_Usage2DEngine);
}

Explorer::~Explorer() {
    this->renderer.unregister_texture(this->file_texture);
    this->renderer.unregister_texture(this->folder_texture);
    this->renderer.unregister_texture(this->recent_texture);
    this->renderer.unregister_texture(this->sd_texture);
    this->renderer.unregister_texture(this->usb_texture);
    this->renderer.unregister_texture(this->network_texture);
}

bool Explorer::update_state(PadState &pad, HidTouchScreenState &touch) {
    if (this->need_directory_scan) {
        this->need_directory_scan = false;
        this->context.cur_path = this->path.base();

        auto *dir = opendir(this->path.c_str());
        if (dir) {
            SW_SCOPEGUARD([dir] { closedir(dir); });
            this->entries.clear();

            auto *reent    = __syscall_getreent();
            auto *devoptab = devoptab_list[dir->dirData->device];

            struct stat st;
            while (true) {
                reent->deviceData = devoptab->deviceData;
                if (devoptab->dirnext_r(reent, dir->dirData, dir->fileData.d_name, &st))
                    break;

                auto path = this->path / dir->fileData.d_name;

                // Strip "recent:/" from path
                if (this->context.cur_fs->type == fs::Filesystem::Type::Recent)
                    path = path.internal().substr(1);

                // In the recent filesystem multiple files might have the same name
                auto name = std::string(path.filename()) + "##" + path.base();

                if (S_ISDIR(st.st_mode))
                    this->entries.emplace_back(fs::Node{fs::Node::Type::Directory, std::move(name)});
                else
                    this->entries.emplace_back(fs::Node{fs::Node::Type::File, std::move(name), std::size_t(st.st_size)});
            }

            if (this->context.cur_fs->type != fs::Filesystem::Type::Recent) {
                std::sort(this->entries.begin(), this->entries.end(), [](const fs::Node &lhs, const fs::Node &rhs) {
                    if (lhs.type != rhs.type)
                        return lhs.type < rhs.type;
                    return strcasecmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
                });
            }

            this->want_focus_reset = !this->is_initial_scan;
            this->is_initial_scan  = false;
        } else {
            std::printf("Failed to open directory %s: %s (%d)\n", this->path.c_str(), std::strerror(errno), errno);
            this->context.set_error(errno);
        }
    }

    return true;
}

void Explorer::render() {
    {
        ImGui::PushItemWidth(this->screen_rel_width(0.15));
        SW_SCOPEGUARD([] { ImGui::PopItemWidth(); });

        if (ImGui::BeginCombo("##fscombo", this->context.cur_fs->name.data())) {
            SW_SCOPEGUARD([] { ImGui::EndCombo(); });

            for (auto &fs: this->context.filesystems) {
                Renderer::Texture *tex;
                switch (fs->type) {
                    using enum fs::Filesystem::Type;
                    default:
                    case Recent:
                        tex = &this->recent_texture;
                        break;
                    case Sdmc:
                        tex = &this->sd_texture;
                        break;
                    case Usb:
                        tex = &this->usb_texture;
                        break;
                    case Network:
                        tex = &this->network_texture;
                        break;
                }

                ImVec4 tint_col = (ImGui::nx::getCurrentTheme() == ColorSetId_Dark) ?
                    ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);

                ImGui::Image(ImGui::deko3d::makeTextureID(tex->handle, true),
                    ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()), ImVec2(0, 0), ImVec2(1, 1), tint_col);

                ImGui::SameLine();
                if (ImGui::Selectable(fs->name.data(), this->context.cur_fs == fs)) {
                    this->context.cur_fs = fs;
                    this->need_directory_scan = true;

                    this->path = fs::Path(this->context.cur_fs->mount_name) + "/";
                }
            }
        }
    }

    bool want_explore_backward = this->is_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft),
        want_explore_forward   = this->is_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight);

    std::string_view path = this->path.internal();

    char buttonstr[50] = {};
    if (path.length() > 43)
        std::snprintf(buttonstr, sizeof(buttonstr), "...%s", utf8_skip_from_end(path, 43).data());
    else
        std::strncpy(buttonstr, path.data(), sizeof(buttonstr)-1);

    {
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5));
        SW_SCOPEGUARD([] { ImGui::PopStyleVar(); });

        ImGui::SameLine();
        want_explore_backward |= ImGui::Button(buttonstr, ImVec2(-1, 0));
    }

    auto reserved_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeightWithSpacing();

    if (ImGui::BeginListBox("##fsentries", ImVec2(-1, -reserved_height))) {
        SW_SCOPEGUARD([] { ImGui::EndListBox(); });

        this->is_focused = ImGui::IsWindowFocused();

        ImVec4 tint_col = (ImGui::nx::getCurrentTheme() == ColorSetId_Dark) ?
            ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);

        ImGuiListClipper clipper;
        clipper.Begin(this->entries.size());

        while (clipper.Step()) {
            for (auto i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                auto &entry = this->entries[i];
                ImGui::Image(ImGui::deko3d::makeTextureID((entry.type == fs::Node::Type::File) ?
                        this->file_texture.handle : this->folder_texture.handle, true),
                    ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()), ImVec2(0, 0), ImVec2(1, 1), tint_col);
                ImGui::SameLine();

                want_explore_forward |= ImGui::Selectable(entry.name.c_str());
                auto is_item_focused = ImGui::IsItemFocused();

                if (is_item_focused)
                    this->cur_focused_entry = i;
            }
        }

        if (want_explore_backward) {
            if (!this->path.is_root())
                this->path = this->path.parent();
            this->need_directory_scan = true;
            this->cur_focused_entry = -1;
        } else if (want_explore_forward && this->cur_focused_entry != -1u) {
            auto &entry = this->entries[this->cur_focused_entry];
            switch (entry.type) {
                case fs::Node::Type::Directory:
                    this->path = Explorer::path_from_entry_name(entry.name);
                    this->need_directory_scan = true;
                    break;
                case fs::Node::Type::File:
                    this->selection = Explorer::path_from_entry_name(entry.name);
                    this->context.cur_file = Explorer::path_from_entry_name(entry.name);
                    break;
            }
        }

        if (this->want_focus_reset && !this->entries.empty()) {
            auto &entry = this->entries.front();
            ImGui::SetNavWindow(ImGui::GetCurrentWindow());
            ImGui::SetNavID(ImGui::GetID(entry.name.c_str()), ImGuiNavLayer_Main, 0, ImRect());
            this->want_focus_reset = false;
        }
    }

    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(this->screen_rel_width(0.2), ImGui::GetStyle().ItemSpacing.y));
    ImGui::Text("Navigate with \ue0ea");
}

} // namespace sw::ui
