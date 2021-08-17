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

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "context.hpp"
#include "fs/fs_common.hpp"

namespace sw::fs {

class SftpFs final: public NetworkFilesystem {
    public:
        SftpFs(Context &context, std::string_view name, std::string_view mount_name);
        virtual ~SftpFs() override;

        virtual int initialize() override;
        virtual int connect(std::string_view host, std::uint16_t port, std::string_view share,
            std::string_view username, std::string_view password) override;
        virtual int disconnect() override;

    private:
        std::string translate_path(const char *path);

        static int       sftp_open    (struct _reent *r, void *fileStruct, const char *path, int flags, int mode);
        static int       sftp_close   (struct _reent *r, void *fd);
        static ssize_t   sftp_read    (struct _reent *r, void *fd, char *ptr, size_t len);
        static off_t     sftp_seek    (struct _reent *r, void *fd, off_t pos, int dir);
        static int       sftp_fstat   (struct _reent *r, void *fd, struct stat *st);
        static int       sftp_stat    (struct _reent *r, const char *file, struct stat *st);
        static int       sftp_chdir   (struct _reent *r, const char *name);
        static DIR_ITER *sftp_diropen (struct _reent *r, DIR_ITER *dirState, const char *path);
        static int       sftp_dirreset(struct _reent *r, DIR_ITER *dirState);
        static int       sftp_dirnext (struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
        static int       sftp_dirclose(struct _reent *r, DIR_ITER *dirState);
        static int       sftp_statvfs (struct _reent *r, const char *path, struct statvfs *buf);
        static int       sftp_lstat   (struct _reent *r, const char *file, struct stat *st);

    private:
        struct SftpFsFile {
            LIBSSH2_SFTP_HANDLE *handle;
            LIBSSH2_SFTP_ATTRIBUTES attrs;
            off_t offset;
        };

        struct SftpFsDir {
            LIBSSH2_SFTP_HANDLE *handle;
        };

    private:
        static inline std::atomic_int lib_refcount = 0;

        Context &context;

        int sock;
        LIBSSH2_SESSION *ssh_session  = nullptr;
        LIBSSH2_SFTP    *sftp_session = nullptr;
        std::mutex session_mutex;

        std::string cwd = "";
};

} // namespace sw::fs
