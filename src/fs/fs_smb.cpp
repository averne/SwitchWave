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

#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/syslimits.h>
#include <netinet/tcp.h>

#include "fs/fs_smb.hpp"
#include <utime.h>

namespace sw::fs {

namespace {

void smb2_translate_stat(struct smb2_stat_64 &smb, struct stat *st) {
    auto translate_mode = [](std::uint32_t type) -> mode_t {
        switch (type) {
            case SMB2_TYPE_FILE:
            default:
                return S_IFREG;
            case SMB2_TYPE_DIRECTORY:
                return S_IFDIR;
            case SMB2_TYPE_LINK:
                return S_IFLNK;
        }
    };

    *st = {
        .st_mode     = translate_mode(smb.smb2_type),
        .st_size     = off_t(smb.smb2_size),
        .st_atim     = {
            .tv_sec  = long(smb.smb2_atime),
            .tv_nsec = long(smb.smb2_atime_nsec),
        },
        .st_mtim = {
            .tv_sec  = long(smb.smb2_mtime),
            .tv_nsec = long(smb.smb2_mtime_nsec),
        },
        .st_ctim = {
            .tv_sec  = long(smb.smb2_ctime),
            .tv_nsec = long(smb.smb2_ctime_nsec),
        },
    };
}

} // namespace

SmbFs::SmbFs(Context &context, std::string_view name, std::string_view mount_name): context(context) {
    this->type       = Filesystem::Type::Network;
    this->name       = name;
    this->mount_name = mount_name;

    this->devoptab = {
        .name         = this->name.data(),

        .structSize   = sizeof(SmbFs),
        .open_r       = SmbFs::smb_open,
        .close_r      = SmbFs::smb_close,
        .read_r       = SmbFs::smb_read,
        .seek_r       = SmbFs::smb_seek,
        .fstat_r      = SmbFs::smb_fstat,

        .stat_r       = SmbFs::smb_stat,
        .chdir_r      = SmbFs::smb_chdir,

        .dirStateSize = sizeof(SmbFs),
        .diropen_r    = SmbFs::smb_diropen,
        .dirreset_r   = SmbFs::smb_dirreset,
        .dirnext_r    = SmbFs::smb_dirnext,
        .dirclose_r   = SmbFs::smb_dirclose,

        .statvfs_r    = SmbFs::smb_statvfs,

        .deviceData   = this,

        .lstat_r      = SmbFs::smb_lstat,
    };
}

SmbFs::~SmbFs() {
    if (this->is_connected)
        this->disconnect();

    this->unregister_fs();
}

int SmbFs::initialize() {
    this->smb_ctx = ::smb2_init_context();
    if (!this->smb_ctx)
        return ENOMEM;

    ::smb2_set_timeout(this->smb_ctx, 3);

    return 0;
}

int SmbFs::connect(std::string_view host, std::uint16_t port, std::string_view share,
        std::string_view username, std::string_view password) {
    if (!username.empty())
        ::smb2_set_user    (this->smb_ctx, username.data());

    if (!password.empty())
        ::smb2_set_password(this->smb_ctx, password.data());

    ::smb2_set_security_mode(this->smb_ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    auto lk = std::scoped_lock(this->session_mutex);

    if (auto rc = ::smb2_connect_share(this->smb_ctx, host.data(), share.data(), nullptr); rc < 0)
        return -rc;

    this->is_connected = true;

    return 0;
}

int SmbFs::disconnect() {
    int rc = 0;

    auto lk = std::scoped_lock(this->session_mutex);

    if (this->smb_ctx) {
        ::smb2_disconnect_share(this->smb_ctx);
        ::smb2_destroy_context(this->smb_ctx);
    }

    this->is_connected = false;

    return rc;
}

std::string SmbFs::translate_path(const char *path) {
    return this->cwd + (path + this->mount_name.length());
}

int SmbFs::smb_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode) {
    auto *priv      = static_cast<SmbFs     *>(r->deviceData);
    auto *priv_file = static_cast<SmbFsFile *>(fileStruct);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    priv_file->handle = ::smb2_open(priv->smb_ctx, internal_path.c_str() + 1, flags);
    if (!priv_file->handle) {
        __errno_r(r) = ENOENT;
        return -1;
    }

    if (auto rc = ::smb2_fstat(priv->smb_ctx, priv_file->handle, &priv_file->stat); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    return 0;
}

int SmbFs::smb_close(struct _reent *r, void *fd) {
    auto *priv      = static_cast<SmbFs     *>(r->deviceData);
    auto *priv_file = static_cast<SmbFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    if (auto rc = ::smb2_close(priv->smb_ctx, priv_file->handle); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    return 0;
}

ssize_t SmbFs::smb_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto *priv      = static_cast<SmbFs     *>(r->deviceData);
    auto *priv_file = static_cast<SmbFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    if (auto rc = ::smb2_read(priv->smb_ctx, priv_file->handle,
            reinterpret_cast<std::uint8_t *>(ptr), len); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    } else {
        return rc;
    }
}

off_t SmbFs::smb_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto *priv      = static_cast<SmbFs     *>(r->deviceData);
    auto *priv_file = static_cast<SmbFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    std::uint64_t absolute;
    if (auto rc = ::smb2_lseek(priv->smb_ctx, priv_file->handle, pos, dir, &absolute); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    } else {
        return absolute;
    }
}

int SmbFs::smb_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto *priv_file = static_cast<SmbFsFile *>(fd);

    smb2_translate_stat(priv_file->stat, st);
    return 0;
}

int SmbFs::smb_stat(struct _reent *r, const char *file, struct stat *st) {
    auto *priv = static_cast<SmbFs *>(r->deviceData);

    auto internal_path = priv->translate_path(file);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    struct smb2_stat_64 buf;
    if (auto rc = ::smb2_stat(priv->smb_ctx, internal_path.c_str() + 1, &buf); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    smb2_translate_stat(buf, st);
    return 0;
}

int SmbFs::smb_lstat(struct _reent *r, const char *file, struct stat *st) {
    auto *priv = static_cast<SmbFs *>(r->deviceData);

    auto internal_path = priv->translate_path(file);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    std::array<char, PATH_MAX> target;
    if (auto rc = ::smb2_readlink(priv->smb_ctx, internal_path.c_str() + 1,
            target.data(), target.size()); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    struct smb2_stat_64 buf;
    if (auto rc = ::smb2_stat(priv->smb_ctx, target.data(), &buf); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    smb2_translate_stat(buf, st);
    return 0;
}

int SmbFs::smb_chdir(struct _reent *r, const char *name) {
    return 0;
}

DIR_ITER *SmbFs::smb_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    auto *priv     = static_cast<SmbFs    *>(r->deviceData);
    auto *priv_dir = static_cast<SmbFsDir *>(dirState->dirStruct);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return nullptr;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    priv_dir->handle = ::smb2_opendir(priv->smb_ctx, internal_path.c_str() + 1);
    if (!priv_dir->handle) {
        __errno_r(r) = ENOENT;
        return nullptr;
    }

    return dirState;
}

int SmbFs::smb_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto *priv     = static_cast<SmbFs    *>(r->deviceData);
    auto *priv_dir = static_cast<SmbFsDir *>(dirState->dirStruct);

    ::smb2_rewinddir(priv->smb_ctx, priv_dir->handle);
    return 0;
}

int SmbFs::smb_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto *priv     = static_cast<SmbFs    *>(r->deviceData);
    auto *priv_dir = static_cast<SmbFsDir *>(dirState->dirStruct);

    struct smb2dirent *node;
    while (true) {
        node = ::smb2_readdir(priv->smb_ctx, priv_dir->handle);
        if (!node) {
            __errno_r(r) = ENOENT;
            return -1;
        }

        auto fname = std::string_view(node->name);
        if (fname != "." && fname != "..")
            break;
    }

    std::strncpy(filename, node->name, NAME_MAX);

    smb2_translate_stat(node->st, filestat);
    return 0;
}

int SmbFs::smb_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto *priv     = static_cast<SmbFs    *>(r->deviceData);
    auto *priv_dir = static_cast<SmbFsDir *>(dirState->dirStruct);

    ::smb2_closedir(priv->smb_ctx, priv_dir->handle);
    return 0;
}

int SmbFs::smb_statvfs(struct _reent *r, const char *path, struct statvfs *buf) {
    auto *priv = static_cast<SmbFs *>(r->deviceData);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    struct smb2_statvfs st;
    if (auto rc = ::smb2_statvfs(priv->smb_ctx, internal_path.c_str() + 1, &st); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    *buf = {};
    buf->f_bsize   = st.f_bsize;
    buf->f_frsize  = st.f_frsize;
    buf->f_blocks  = st.f_blocks;
    buf->f_bfree   = st.f_bfree;
    buf->f_bavail  = st.f_bavail;
    buf->f_files   = st.f_files;
    buf->f_ffree   = st.f_ffree;
    buf->f_favail  = st.f_favail;
    buf->f_fsid    = st.f_fsid;
    buf->f_flag    = st.f_flag;
    buf->f_namemax = st.f_namemax;

    return 0;
}

} // namespace sw::fs
