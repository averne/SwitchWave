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

#include <list>
#include <string_view>
#include <unordered_map>

#include <switch.h>

#include "context.hpp"
#include "fs/fs_common.hpp"

namespace sw::fs {

class RecentFs final: public Filesystem {
    public:
        RecentFs(Context &context, std::string_view name, std::string_view mount_name);
        virtual ~RecentFs() override;

        void add(const std::string &path);
        void clear() {
            this->recent_files.clear();
        }

        int write_to_file() const;

    private:
        struct RecentFsDir {
            std::list<Path>::iterator it;
        };

    private:
        static DIR_ITER *recent_diropen (struct _reent *r, DIR_ITER *dirState, const char *path);
        static int       recent_dirreset(struct _reent *r, DIR_ITER *dirState);
        static int       recent_dirnext (struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
        static int       recent_dirclose(struct _reent *r, DIR_ITER *dirState);

    private:
        Context &context;

        Path history_path;
        std::list<Path> recent_files;
};

} // namespace sw::fs
