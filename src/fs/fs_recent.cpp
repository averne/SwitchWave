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

#include <sys/syslimits.h>

#include <switch.h>

#include "utils.hpp"
#include "fs/fs_common.hpp"
#include "fs/fs_recent.hpp"

namespace sw::fs {

RecentFs::RecentFs(Context &context, std::string_view name, std::string_view mount_name): context(context) {
    this->type       = Filesystem::Type::Recent;
    this->name       = name;
    this->mount_name = mount_name;

    this->devoptab = {
        .name         = this->name.data(),

        .dirStateSize = sizeof(RecentFsDir),
        .diropen_r    = RecentFs::recent_diropen,
        .dirreset_r   = RecentFs::recent_dirreset,
        .dirnext_r    = RecentFs::recent_dirnext,
        .dirclose_r   = RecentFs::recent_dirclose,

        .deviceData   = this,
    };

    this->history_path = Path(Context::AppDirectory) / Context::HistoryFilename;

    std::string text;
    utils::read_whole_file(text, this->history_path.c_str(), "r");
    if (!text.empty()) {
        auto *start = text.c_str();
        while (const auto *end = std::strchr(start, '\n')) {
            if (this->recent_files.size() >= this->context.history_size)
                break;

            SW_SCOPEGUARD([&] { start = end + 1; });
            if (start == end)
                continue;

            this->recent_files.emplace_back(start, end);
        }
    }
}

RecentFs::~RecentFs() {
    this->unregister_fs();
}

void RecentFs::add(const std::string &path) {
    // Remove duplicates
    auto hash = std::hash<std::string>{}(path);
    this->recent_files.remove_if([hash](const auto &path) { return std::hash<std::string>{}(path.base()) == hash; });

    this->recent_files.emplace_front(path);

    if (this->recent_files.size() > this->context.history_size)
        this->recent_files.resize(this->context.history_size);
}

int RecentFs::write_to_file() const {
    auto *fp = std::fopen(this->history_path.c_str(), "w");
    if (!fp)
        return -1;
    SW_SCOPEGUARD([&fp] { std::fclose(fp); });

    for (auto &path: this->recent_files) {
        auto &str = path.base();
        if (std::size_t written = std::fprintf(fp, "%s\n", str.c_str()); written != str.length() + 1)
            return -1;
    }

    return 0;
}

DIR_ITER* RecentFs::recent_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    auto *priv     = static_cast<RecentFs    *>(r->deviceData);
    auto *priv_dir = static_cast<RecentFsDir *>(dirState->dirStruct);

    auto internal_path = Path::internal(path);
    if (internal_path.empty())
        return nullptr;

    if (internal_path != "/")
        return nullptr;

    priv_dir->it = priv->recent_files.begin();

    return dirState;
}

int RecentFs::recent_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto *priv     = static_cast<RecentFs    *>(r->deviceData);
    auto *priv_dir = static_cast<RecentFsDir *>(dirState->dirStruct);

    priv_dir->it = priv->recent_files.begin();

    return -1;
}

int RecentFs::recent_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto *priv     = static_cast<RecentFs    *>(r->deviceData);
    auto *priv_dir = static_cast<RecentFsDir *>(dirState->dirStruct);

    if (priv_dir->it != priv->recent_files.end()) {
        auto &path = *priv_dir->it;

        std::strncpy(filename, path.c_str(), NAME_MAX);
        ::stat(path.c_str(), filestat);

        ++priv_dir->it;
    } else {
        __errno_r(r) = ENOENT;
        return -1;
    }

    return 0;
}

int RecentFs::recent_dirclose(struct _reent *r, DIR_ITER *dirState) {
    return 0;
}

} // namespace sw::fs
