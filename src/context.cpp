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
#include <algorithm>

#include <ini.h>

#include "utils.hpp"
#include "fs/fs_common.hpp"
#include "fs/fs_smb.hpp"
#include "fs/fs_nfs.hpp"
#include "fs/fs_sftp.hpp"

#include "context.hpp"

namespace sw {

int Context::read_from_file() {
    for (auto &info: this->network_infos)
        this->unregister_network_fs(*info);

    std::string ini_text;
    if (utils::read_whole_file(ini_text, this->config_path.c_str(), "r")) {
        std::printf("Failed to read %s\n", this->config_path.c_str());
        return -1;
    }

    ini_handler ini_parse_callback = +[](void *user, const char *section,
            const char *name, const char *value) {
        auto self = static_cast<Context *>(user);

        auto s = std::string_view(section), n = std::string_view(name),
            v = std::string_view(value);

        if (s.length() == 0) {
            if (n == "fast-presentation")
                self->use_fast_presentation = v != "no";
            else if (n == "disable-screensaver")
                self->disable_screensaver   = v != "no";
            else if (n == "quit-to-home-menu")
                self->quit_to_home_menu     = v != "no";
            else if (n == "override-screenshot-button")
                self->override_screenshot_button = v != "no";
            else if (n == "history-size")
                self->history_size = std::atoi(v.data());
        } else if (s.find("network") != std::string_view::npos) {
            auto name = s.substr(s.find(':')+1);

            auto it = std::find_if(self->network_infos.begin(), self->network_infos.end(), [&name](const auto &info) {
                return info->fs_name == name;
            });

            auto &info = (it != self->network_infos.end()) ? *it :
                self->network_infos.emplace_back(std::make_unique<NetworkFsInfo>(NetworkFsInfo{ .fs_name = name.data() }));

            if (n == "protocol")
                info->protocol = (v == "smb" ? fs::NetworkFilesystem::Protocol::Smb :
                    (v == "nfs" ? fs::NetworkFilesystem::Protocol::Nfs : fs::NetworkFilesystem::Protocol::Sftp));
            else if (n == "connect")
                info->want_connect = v != "no";
            else if (n == "share")
                info->share = v;
            else if (n == "mountpoint")
                info->mountpoint = v;
            else if (n == "host")
                info->host = v;
            else if (n == "port")
                info->port = v;
            else if (n == "username")
                info->username = v;
            else if (n == "password")
                info->password = v;
        } else {
            std::printf("Unknown ini key [%s]%s = %s\n", s.data(), n.data(), v.data());
        }

        return 0;
    };

    if (ini_parse_string(ini_text.c_str(), ini_parse_callback, this) < 0) {
        std::printf("Failed to parse configuration\n");
        return -1;
    }

    return 0;
}

int Context::write_to_file() {
    auto *fp = std::fopen(this->config_path.c_str(), "w");
    if (!fp) {
        std::printf("Failed to open %s\n", this->config_path.c_str());
        this->set_error(errno);
        return 1;
    }
    SW_SCOPEGUARD([&fp] { std::fclose(fp); });

#define TRY_WRITE(expr) ({          \
    if (auto rc = (expr); rc < 0) { \
        this->set_error(errno);     \
        return -1;                  \
    }                               \
})

    TRY_WRITE(std::fprintf(fp, "%s = %s\n",  "fast-presentation",          this->use_fast_presentation      ? "yes" : "no"));
    TRY_WRITE(std::fprintf(fp, "%s = %s\n",  "disable-screensaver",        this->disable_screensaver        ? "yes" : "no"));
    TRY_WRITE(std::fprintf(fp, "%s = %s\n",  "quit-to-home-menu",          this->quit_to_home_menu          ? "yes" : "no"));
    TRY_WRITE(std::fprintf(fp, "%s = %s\n",  "override-screenshot-button", this->override_screenshot_button ? "yes" : "no"));
    TRY_WRITE(std::fprintf(fp, "%s = %ld\n", "history-size",               this->history_size));

    for (auto &info: this->network_infos) {
        TRY_WRITE(std::fprintf(fp, "[network:%s]\n",    info->fs_name   .c_str()));
        TRY_WRITE(std::fprintf(fp, "protocol = %s\n",   fs::NetworkFilesystem::protocol_name(info->protocol).data()));
        TRY_WRITE(std::fprintf(fp, "connect = %s\n",    (info->fs && info->fs->connected()) ? "yes" : "no"));
        TRY_WRITE(std::fprintf(fp, "share = %s\n",      info->share     .c_str()));
        TRY_WRITE(std::fprintf(fp, "mountpoint = %s\n", info->mountpoint.c_str()));
        TRY_WRITE(std::fprintf(fp, "host = %s\n",       info->host      .c_str()));
        TRY_WRITE(std::fprintf(fp, "port = %s\n",       info->port      .c_str()));
        TRY_WRITE(std::fprintf(fp, "username = %s\n",   info->username  .c_str()));
        TRY_WRITE(std::fprintf(fp, "password = %s\n",   info->password  .c_str()));
    }

    return 0;
}

int Context::register_network_fs(NetworkFsInfo &info) {
    std::shared_ptr<fs::NetworkFilesystem> fs;

    info.mountpoint = info.fs_name + ":";

    switch (info.protocol) {
        case fs::NetworkFilesystem::Protocol::Nfs:
            fs = std::make_shared<fs::NfsFs>(*this, info.fs_name, info.mountpoint);
            break;
        case fs::NetworkFilesystem::Protocol::Smb:
            fs = std::make_shared<fs::SmbFs>(*this, info.fs_name, info.mountpoint);
            break;
        case fs::NetworkFilesystem::Protocol::Sftp:
            fs = std::make_shared<fs::SftpFs>(*this, info.fs_name, info.mountpoint);
            break;
        default:
            return -1;
    }

    if (auto rc = fs->initialize(); rc)
        return rc;

    if (auto rc = fs->connect(info.host, std::atoi(info.port.c_str()),
            info.share, info.username, info.password); rc)
        return rc;

    if (auto rc = fs->register_fs(); rc)
        return rc;

    info.fs = fs;

    this->filesystems.emplace_back(std::move(fs));

    return 0;
}

int Context::unregister_network_fs(NetworkFsInfo &info) {
    int rc = 0;

    if (!info.fs)
        return rc;

    if (info.fs->connected())
        rc |= info.fs->disconnect();

    if (this->cur_fs == info.fs)
        this->cur_fs = this->filesystems.front();

    std::erase(this->filesystems, info.fs);
    info.fs.reset();

    return rc;
}

} // namespace sw
