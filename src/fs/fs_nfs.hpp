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

#include <nfsc/libnfs.h>

#include "context.hpp"
#include "fs/fs_common.hpp"

namespace sw::fs {

class NfsFs final: public NetworkFilesystem {
    public:
        NfsFs(Context &context, std::string_view name, std::string_view mount_name);
        virtual ~NfsFs() override;

        virtual int initialize() override;
        virtual int connect(std::string_view host, std::uint16_t port, std::string_view share,
            std::string_view username, std::string_view password) override;
        virtual int disconnect() override;

    private:
        std::string_view translate_path(const char *path);

        static int       nfs_open    (struct _reent *r, void *fileStruct, const char *path, int flags, int mode);
        static int       nfs_close   (struct _reent *r, void *fd);
        static ssize_t   nfs_read    (struct _reent *r, void *fd, char *ptr, size_t len);
        static off_t     nfs_seek    (struct _reent *r, void *fd, off_t pos, int dir);
        static int       nfs_fstat   (struct _reent *r, void *fd, struct stat *st);
        static int       nfs_stat    (struct _reent *r, const char *file, struct stat *st);
        static int       nfs_chdir   (struct _reent *r, const char *name);
        static DIR_ITER *nfs_diropen (struct _reent *r, DIR_ITER *dirState, const char *path);
        static int       nfs_dirreset(struct _reent *r, DIR_ITER *dirState);
        static int       nfs_dirnext (struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
        static int       nfs_dirclose(struct _reent *r, DIR_ITER *dirState);
        static int       nfs_statvfs (struct _reent *r, const char *path, struct statvfs *buf);
        static int       nfs_lstat   (struct _reent *r, const char *file, struct stat *st);

    private:
        struct NfsFsFile {
            struct nfsfh *handle;
            struct nfs_stat_64 stat;
        };

        struct NfsFsDir {
            struct nfsdir *handle;
        };

    private:
        Context &context;

        nfs_context *nfs_ctx = nullptr;

        std::mutex session_mutex;
};

} // namespace sw::fs
