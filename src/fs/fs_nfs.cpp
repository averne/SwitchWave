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

#include <nfsc/libnfs-raw-mount.h>

#include "fs/fs_nfs.hpp"

namespace sw::fs {

namespace {

void nfs_translate_stat(struct nfs_stat_64 &nfs, struct stat *st) {
    *st = {
        .st_mode     = mode_t(nfs.nfs_mode),
        .st_uid      =  uid_t(nfs.nfs_uid),
        .st_gid      =  gid_t(nfs.nfs_gid),
        .st_size     =  off_t(nfs.nfs_size),
        .st_atim     = {
            .tv_sec  = long(nfs.nfs_atime),
            .tv_nsec = long(nfs.nfs_atime_nsec),
        },
        .st_mtim = {
            .tv_sec  = long(nfs.nfs_mtime),
            .tv_nsec = long(nfs.nfs_mtime_nsec),
        },
        .st_ctim = {
            .tv_sec  = long(nfs.nfs_ctime),
            .tv_nsec = long(nfs.nfs_ctime_nsec),
        },
    };
}

} // namespace

NfsFs::NfsFs(Context &context, std::string_view name, std::string_view mount_name): context(context) {
    this->type       = Filesystem::Type::Network;
    this->name       = name;
    this->mount_name = mount_name;

    this->devoptab = {
        .name         = this->name.data(),

        .structSize   = sizeof(NfsFsFile),
        .open_r       = NfsFs::nfs_open,
        .close_r      = NfsFs::nfs_close,
        .read_r       = NfsFs::nfs_read,
        .seek_r       = NfsFs::nfs_seek,
        .fstat_r      = NfsFs::nfs_fstat,

        .stat_r       = NfsFs::nfs_stat,
        .chdir_r      = NfsFs::nfs_chdir,

        .dirStateSize = sizeof(NfsFsDir),
        .diropen_r    = NfsFs::nfs_diropen,
        .dirreset_r   = NfsFs::nfs_dirreset,
        .dirnext_r    = NfsFs::nfs_dirnext,
        .dirclose_r   = NfsFs::nfs_dirclose,

        .statvfs_r    = NfsFs::nfs_statvfs,

        .deviceData   = this,

        .lstat_r      = NfsFs::nfs_lstat,
    };
}

NfsFs::~NfsFs() {
    if (this->is_connected)
        this->disconnect();

    this->unregister_fs();
}

int NfsFs::initialize() {
    this->nfs_ctx = ::nfs_init_context();
    if (!this->nfs_ctx)
        return ENOMEM;

    ::nfs_set_timeout(this->nfs_ctx, 3000);

    return 0;
}

int NfsFs::connect(std::string_view host, std::uint16_t port, std::string_view share,
        std::string_view username, std::string_view password) {
    // auto exports = ::mount_getexports(host.data());
    // SW_SCOPEGUARD([&exports] { ::mount_free_export_list(exports); });
    // while (exports) {
    //     printf("Found export %s\n", exports->ex_dir);
    //     exports = exports->ex_next;
    // }

    auto lk = std::scoped_lock(this->session_mutex);

    if (auto rc = ::nfs_mount(this->nfs_ctx, host.data(), share.data()); rc < 0)
        return -rc;

    this->is_connected = true;

    return 0;
}

int NfsFs::disconnect() {
    int rc = 0;

    auto lk = std::scoped_lock(this->session_mutex);

    if (this->nfs_ctx)
        ::nfs_destroy_context(this->nfs_ctx);

    this->is_connected = false;

    return rc;
}

std::string_view NfsFs::translate_path(const char *path) {
    return path + this->mount_name.length();
}

int NfsFs::nfs_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode) {
    auto *priv      = static_cast<NfsFs     *>(r->deviceData);
    auto *priv_file = static_cast<NfsFsFile *>(fileStruct);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    if (auto rc = ::nfs_open2(priv->nfs_ctx, internal_path.data(),
            flags, mode, &priv_file->handle); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    if (auto rc = ::nfs_fstat64(priv->nfs_ctx, priv_file->handle, &priv_file->stat); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    return 0;
}

int NfsFs::nfs_close(struct _reent *r, void *fd) {
    auto *priv      = static_cast<NfsFs     *>(r->deviceData);
    auto *priv_file = static_cast<NfsFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    if (auto rc = ::nfs_close(priv->nfs_ctx, priv_file->handle); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    return 0;
}

ssize_t NfsFs::nfs_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto *priv      = static_cast<NfsFs     *>(r->deviceData);
    auto *priv_file = static_cast<NfsFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    if (auto rc = ::nfs_read(priv->nfs_ctx, priv_file->handle, len, ptr); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    } else {
        return rc;
    }
}

off_t NfsFs::nfs_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto *priv      = static_cast<NfsFs     *>(r->deviceData);
    auto *priv_file = static_cast<NfsFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    std::uint64_t absolute;
    if (auto rc = ::nfs_lseek(priv->nfs_ctx, priv_file->handle, pos, dir, &absolute); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    } else {
        return absolute;
    }
}

int NfsFs::nfs_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto *priv_file = static_cast<NfsFsFile *>(fd);

    nfs_translate_stat(priv_file->stat, st);
    return 0;
}

int NfsFs::nfs_stat(struct _reent *r, const char *file, struct stat *st) {
    auto *priv = static_cast<NfsFs *>(r->deviceData);

    auto internal_path = priv->translate_path(file);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    struct nfs_stat_64 buf;
    if (auto rc = ::nfs_stat64(priv->nfs_ctx, internal_path.data(), &buf); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    nfs_translate_stat(buf, st);
    return 0;
}

int NfsFs::nfs_lstat(struct _reent *r, const char *file, struct stat *st) {
    auto *priv = static_cast<NfsFs *>(r->deviceData);

    auto internal_path = priv->translate_path(file);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    struct nfs_stat_64 buf;
    if (auto rc = ::nfs_lstat64(priv->nfs_ctx, internal_path.data(), &buf); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    nfs_translate_stat(buf, st);
    return 0;
}

int NfsFs::nfs_chdir(struct _reent *r, const char *name) {
    auto *priv = static_cast<NfsFs *>(r->deviceData);

    auto internal_path = priv->translate_path(name);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    if (auto rc = ::nfs_chdir(priv->nfs_ctx, internal_path.data()); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    return 0;
}

DIR_ITER *NfsFs::nfs_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    auto *priv     = static_cast<NfsFs    *>(r->deviceData);
    auto *priv_dir = static_cast<NfsFsDir *>(dirState->dirStruct);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return nullptr;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    auto rc = ::nfs_opendir(priv->nfs_ctx, internal_path.data(), &priv_dir->handle);
    if (!priv_dir->handle) {
        __errno_r(r) = -rc;
        return nullptr;
    }

    return dirState;
}

int NfsFs::nfs_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto *priv     = static_cast<NfsFs    *>(r->deviceData);
    auto *priv_dir = static_cast<NfsFsDir *>(dirState->dirStruct);

    ::nfs_rewinddir(priv->nfs_ctx, priv_dir->handle);
    return 0;
}

int NfsFs::nfs_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto *priv     = static_cast<NfsFs    *>(r->deviceData);
    auto *priv_dir = static_cast<NfsFsDir *>(dirState->dirStruct);

    struct nfsdirent *node;
    while (true) {
        node = ::nfs_readdir(priv->nfs_ctx, priv_dir->handle);
        if (!node) {
            __errno_r(r) = ENOENT;
            return -1;
        }

        auto fname = std::string_view(node->name);
        if (fname != "." && fname != "..")
            break;
    }

    std::strncpy(filename, node->name, NAME_MAX);

    *filestat = {
        .st_mode     = mode_t(node->mode),
        .st_uid      =  uid_t(node->uid),
        .st_gid      =  gid_t(node->gid),
        .st_size     =  off_t(node->size),
        .st_atim     = {
            .tv_sec  = node->atime.tv_sec,
            .tv_nsec = node->atime_nsec,
        },
        .st_mtim = {
            .tv_sec  = node->mtime.tv_sec,
            .tv_nsec = node->mtime_nsec,
        },
        .st_ctim = {
            .tv_sec  = node->ctime.tv_sec,
            .tv_nsec = node->ctime_nsec,
        },
    };

    return 0;
}

int NfsFs::nfs_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto *priv     = static_cast<NfsFs    *>(r->deviceData);
    auto *priv_dir = static_cast<NfsFsDir *>(dirState->dirStruct);

    ::nfs_closedir(priv->nfs_ctx, priv_dir->handle);
    return 0;
}

int NfsFs::nfs_statvfs(struct _reent *r, const char *path, struct statvfs *buf) {
    auto *priv = static_cast<NfsFs *>(r->deviceData);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    if (auto rc = ::nfs_statvfs(priv->nfs_ctx, internal_path.data(), buf); rc < 0) {
        __errno_r(r) = -rc;
        return -1;
    }

    return 0;
}

} // namespace sw::fs
