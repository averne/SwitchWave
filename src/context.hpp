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

#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include "utils.hpp"
#include "fs/fs_common.hpp"
#include "fs/fs_ums.hpp"

namespace sw {

class Context {
    public:
        constexpr static std::string_view AppDirectory     = "sdmc:/switch/SwitchWave";
        constexpr static std::string_view SettingsFilename = "SwitchWave.conf";
        constexpr static std::string_view HistoryFilename  = "history.txt";

    public:
        enum ErrorType {
            Io,
            Network,
            Mpv,
            LibAv,
            AppletMode,
        };

        struct NetworkFsInfo {
            bool want_connect = false;
            fs::NetworkFilesystem::Protocol protocol;
            utils::StaticString32 host;
            utils::StaticString32 port;
            utils::StaticString32 share;
            utils::StaticString32 username, password;
            utils::StaticString32 fs_name, mountpoint;
            std::shared_ptr<fs::NetworkFilesystem> fs;
        };

    // Settings
    public:
        int read_from_file();
        int write_to_file();

        bool use_fast_presentation      = false;
        bool disable_screensaver        = true;
        bool override_screenshot_button = false;
        bool quit_to_home_menu          = false;

        std::size_t history_size = 50;
        std::string cur_path;

    // Context
    public:
        bool want_quit = false, cli_mode = false;
        bool playback_started, player_is_idle;

        int last_error            = 0;
        ErrorType last_error_type = ErrorType::Io;

        std::string cur_file;

    // Filesystem management
    public:
        int register_network_fs  (NetworkFsInfo &info);
        int unregister_network_fs(NetworkFsInfo &info);

        inline void set_error(int error, Context::ErrorType type = Context::ErrorType::Io) {
            this->last_error      = error;
            this->last_error_type = type;
        }

        inline const fs::Filesystem *get_filesystem(std::string_view mountpoint) const {
            auto it = std::find_if(this->filesystems.begin(), this->filesystems.end(), [&mountpoint](const auto &fs) {
                return fs->mount_name == mountpoint;
            });
            if (it == this->filesystems.end())
                return nullptr;
            return it->get();
        }

        inline const fs::Filesystem *get_filesystem(const fs::Path &path) const {
            return this->get_filesystem(path.mountpoint());
        }

        std::vector<std::shared_ptr<fs::Filesystem>> filesystems;
        std::shared_ptr<fs::Filesystem> cur_fs;
        std::vector<std::unique_ptr<NetworkFsInfo>> network_infos;

        fs::UmsController ums;

    private:
        static inline auto config_path = fs::Path(Context::AppDirectory) / Context::SettingsFilename;
};

} // namespace sw
