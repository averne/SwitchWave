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

#include "context.hpp"
#include "fs/fs_common.hpp"

namespace sw::fs {

class HttpFs final: public NetworkFilesystem {
    public:
        HttpFs(Context &context, std::string_view name, std::string_view mount_name);
        virtual ~HttpFs() override;

        virtual int initialize() override;
        virtual int connect(std::string_view host, std::uint16_t port, std::string_view share,
            std::string_view username, std::string_view password) override;
        virtual int disconnect() override;

        std::string make_url(std::string_view path) const;

    private:
        std::string translate_path(const char *path);
        void setup_curl_handle(void *curl);

        static int       http_open    (struct _reent *r, void *fileStruct, const char *path, int flags, int mode);
        static int       http_close   (struct _reent *r, void *fd);
        static ssize_t   http_read    (struct _reent *r, void *fd, char *ptr, size_t len);
        static off_t     http_seek    (struct _reent *r, void *fd, off_t pos, int dir);
        static int       http_fstat   (struct _reent *r, void *fd, struct stat *st);
        static int       http_stat    (struct _reent *r, const char *file, struct stat *st);
        static DIR_ITER *http_diropen (struct _reent *r, DIR_ITER *dirState, const char *path);
        static int       http_dirreset(struct _reent *r, DIR_ITER *dirState);
        static int       http_dirnext (struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat);
        static int       http_dirclose(struct _reent *r, DIR_ITER *dirState);
        static int       http_lstat   (struct _reent *r, const char *file, struct stat *st);

    private:
        struct HttpFsDir {
            void *data;
        };

    private:
        static inline std::atomic_int lib_refcount = 0;

        Context &context;

        std::string base_url;
        std::string userpwd;
        std::string auth_url_prefix;

        std::string cwd = "";

        std::mutex session_mutex;
};

} // namespace sw::fs
