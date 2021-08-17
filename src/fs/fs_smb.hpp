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

#include <mutex>
#include <string>
#include <string_view>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "context.hpp"
#include "fs/fs_common.hpp"

namespace sw::fs {

class SmbFs final: public NetworkFilesystem {
    public:
        SmbFs(Context &context, std::string_view name, std::string_view mount_name);
        virtual ~SmbFs() override;

        virtual int initialize() override;
        virtual int connect(std::string_view host, std::uint16_t port, std::string_view share,
            std::string_view username, std::string_view password) override;
        virtual int disconnect() override;

    private:
        std::string translate_path(const char *path);

        static int       smb_open    (struct _reent *r, void *fileStruct, const char *path, int flags, int mode);
        static int       smb_close   (struct _reent *r, void *fd);
        static ssize_t   smb_read    (struct _reent *r, void *fd, char *ptr, size_t len);
        static off_t     smb_seek    (struct _reent *r, void *fd, off_t pos, int dir);
        static int       smb_fstat   (struct _reent *r, void *fd, struct stat *st);
        static int       smb_stat    (struct _reent *r, const char *file, struct stat *st);
        static int       smb_chdir   (struct _reent *r, const char *name);
        static DIR_ITER *smb_diropen (struct _reent *r, DIR_ITER *dirState, const char *path);
        static int       smb_dirreset(struct _reent *r, DIR_ITER *dirState);
        static int       smb_dirnext (struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
        static int       smb_dirclose(struct _reent *r, DIR_ITER *dirState);
        static int       smb_statvfs (struct _reent *r, const char *path, struct statvfs *buf);
        static int       smb_lstat   (struct _reent *r, const char *file, struct stat *st);

    private:
        struct SmbFsFile {
            struct smb2fh *handle;
            struct smb2_stat_64 stat;
        };

        struct SmbFsDir {
            struct smb2dir *handle;
        };

    // private:
    public:
        Context &context;

        smb2_context *smb_ctx = nullptr;

        std::string cwd = "";

        std::mutex session_mutex;
};

} // namespace sw::fs
