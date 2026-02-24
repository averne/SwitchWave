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
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/syslimits.h>

#include "fs/fs_sftp.hpp"

namespace sw::fs {

namespace {

int ssh2_translate_addrinfo_error(int error) {
    switch (error) {
        case 0:
            return 0;
        default:
            return EIO;
        case EAI_SYSTEM:
            return errno ? errno : EIO;
        case EAI_AGAIN:
            return EAGAIN;
        case EAI_BADFLAGS:
            return EINVAL;
        case EAI_FAIL:
            return EHOSTUNREACH;
        case EAI_FAMILY:
            return EAFNOSUPPORT;
        case EAI_MEMORY:
            return ENOMEM;
        case EAI_NONAME:
            return ENOENT;
        case EAI_SERVICE:
            return EPROTONOSUPPORT;
        case EAI_SOCKTYPE:
            return ENOTSUP;
        case EAI_BADHINTS:
            return EINVAL;
        case EAI_PROTOCOL:
            return EPROTONOSUPPORT;
        case EAI_OVERFLOW:
            return ENAMETOOLONG;
    }
}

int ssh2_translate_error(int error, LIBSSH2_SFTP *sftp_session) {
    switch (error) {
        case LIBSSH2_ERROR_NONE:
            return 0;
        case LIBSSH2_ERROR_ALLOC:
            return ENOMEM;
        case LIBSSH2_ERROR_BANNER_SEND:
        case LIBSSH2_ERROR_SOCKET_SEND:
        default:
            return EIO;
        case LIBSSH2_ERROR_SOCKET_TIMEOUT:
            return ETIMEDOUT;
        case LIBSSH2_ERROR_EAGAIN:
            return EAGAIN;
        case LIBSSH2_ERROR_SOCKET_NONE:
        case LIBSSH2_ERROR_SOCKET_DISCONNECT:
            return ENOTSOCK;
        case LIBSSH2_ERROR_KEX_FAILURE:
            return ECONNABORTED;
        case LIBSSH2_ERROR_PROTO:
            return ENOPROTOOPT;
        case LIBSSH2_ERROR_PASSWORD_EXPIRED:
        case LIBSSH2_ERROR_AUTHENTICATION_FAILED:
            return EPERM;
        case LIBSSH2_ERROR_SFTP_PROTOCOL:
            switch (::libssh2_sftp_last_error(sftp_session)) {
                case LIBSSH2_FX_OK:
                    return 0;
                case LIBSSH2_FX_EOF:
                case LIBSSH2_FX_FAILURE:
                case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
                case LIBSSH2_FX_QUOTA_EXCEEDED:
                case LIBSSH2_FX_UNKNOWN_PRINCIPAL:
                default:
                    return EIO;
                case LIBSSH2_FX_NO_SUCH_FILE:
                case LIBSSH2_FX_NO_SUCH_PATH:
                case LIBSSH2_FX_NO_MEDIA:
                    return ENOENT;
                case LIBSSH2_FX_PERMISSION_DENIED:
                case LIBSSH2_FX_WRITE_PROTECT:
                    return EPERM;
                case LIBSSH2_FX_BAD_MESSAGE:
                case LIBSSH2_FX_INVALID_HANDLE:
                case LIBSSH2_FX_INVALID_FILENAME:
                    return EINVAL;
                case LIBSSH2_FX_NO_CONNECTION:
                case LIBSSH2_FX_CONNECTION_LOST:
                    return ECONNRESET;
                case LIBSSH2_FX_OP_UNSUPPORTED:
                    return ENOTSUP;
                case LIBSSH2_FX_FILE_ALREADY_EXISTS:
                    return EEXIST;
                case LIBSSH2_FX_LOCK_CONFLICT:
                    return EDEADLK;
                case LIBSSH2_FX_DIR_NOT_EMPTY:
                    return ENOTEMPTY;
                case LIBSSH2_FX_NOT_A_DIRECTORY:
                    return ENOTDIR;
                case LIBSSH2_FX_LINK_LOOP:
                    return ELOOP;
            }
    }
}

int ssh2_translate_open_flags(int flags) {
    int ssh_flags = 0;

    switch (flags & O_ACCMODE) {
        default:
        case O_RDONLY:
            ssh_flags |= LIBSSH2_FXF_READ;
            break;
        case O_WRONLY:
            ssh_flags |= LIBSSH2_FXF_WRITE;
            break;
        case O_RDWR:
            ssh_flags |= LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE;
            break;
    }

    if (flags & O_CREAT)
        ssh_flags |= LIBSSH2_FXF_CREAT;

    if (flags & O_TRUNC)
        ssh_flags |= LIBSSH2_FXF_TRUNC;

    if (flags & O_EXCL)
        ssh_flags |= LIBSSH2_FXF_EXCL;

    return ssh_flags;
}

void ssh2_translate_stat(LIBSSH2_SFTP_ATTRIBUTES &attrs, struct stat *st) {
    *st = {};

    if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
        st->st_size = attrs.filesize;

    if (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID)
        st->st_uid = attrs.uid, st->st_gid = attrs.gid;

    if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
        st->st_mode = attrs.permissions;

    if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
        st->st_atim.tv_sec = attrs.atime, st->st_mtim.tv_sec = attrs.mtime;

    st->st_nlink = 1;
}

} // namespace

SftpFs::SftpFs(Context &context, std::string_view name, std::string_view mount_name): context(context) {
    this->type       = Filesystem::Type::Network;
    this->name       = name;
    this->mount_name = mount_name;

    this->devoptab = {
        .name         = this->name.data(),

        .structSize   = sizeof(SftpFsFile),
        .open_r       = SftpFs::sftp_open,
        .close_r      = SftpFs::sftp_close,
        .read_r       = SftpFs::sftp_read,
        .seek_r       = SftpFs::sftp_seek,
        .fstat_r      = SftpFs::sftp_fstat,

        .stat_r       = SftpFs::sftp_stat,
        .chdir_r      = SftpFs::sftp_chdir,

        .dirStateSize = sizeof(SftpFsDir),
        .diropen_r    = SftpFs::sftp_diropen,
        .dirreset_r   = SftpFs::sftp_dirreset,
        .dirnext_r    = SftpFs::sftp_dirnext,
        .dirclose_r   = SftpFs::sftp_dirclose,

        .statvfs_r    = SftpFs::sftp_statvfs,

        .deviceData   = this,

        .lstat_r      = SftpFs::sftp_lstat,
    };
}

SftpFs::~SftpFs() {
    if (this->is_connected)
        this->disconnect();

    if (this->ssh_session)
        ::libssh2_session_free(this->ssh_session);

    if (--SftpFs::lib_refcount == 0)
        ::libssh2_exit();

    this->unregister_fs();
}

int SftpFs::initialize() {
    if (SftpFs::lib_refcount++ == 0) {
        if (auto rc = ::libssh2_init(0); rc)
            return ssh2_translate_error(rc, nullptr);
    }

    this->ssh_session = ::libssh2_session_init();
    if (!this->ssh_session)
        return ENOMEM;

    return 0;
}

int SftpFs::connect(std::string_view host, std::uint16_t port, std::string_view share,
        std::string_view username, std::string_view password) {
    struct addrinfo *ai = nullptr;
    SW_SCOPEGUARD([&ai] { ::freeaddrinfo(ai); });

    if (auto rc = ::getaddrinfo(host.data(), nullptr, nullptr, &ai); rc)
        return ssh2_translate_addrinfo_error(rc);

    this->sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (this->sock < 0)
        return errno;

    // Set socket to non-blocking to avoid hangs if the host isn't found
    auto flags = ::fcntl(this->sock, F_GETFL, 0);
    fcntl(this->sock, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in sin = {
        .sin_family = static_cast<sa_family_t>(ai->ai_family),
        .sin_port   = htons(port),
        .sin_addr   = reinterpret_cast<struct sockaddr_in *>(ai->ai_addr)->sin_addr,
    };

    if (auto rc = ::connect(this->sock, reinterpret_cast<sockaddr *>(&sin), sizeof(sin)); rc) {
        if (errno == EAGAIN || errno == EINPROGRESS) {
            pollfd pollfd = {
                .fd     = this->sock,
                .events = POLLOUT,
            };

            rc = ::poll(&pollfd, 1, 3000);
            if (rc > 0) {
                socklen_t len = sizeof(rc);
                ::getsockopt(this->sock, SOL_SOCKET, SO_ERROR, &rc, &len);
            } else {
                rc = ETIMEDOUT;
            }

            if (rc)
                return rc;
        }
    }

    ::fcntl(this->sock, F_SETFL, flags);

    auto lk = std::scoped_lock(this->session_mutex);

    if (auto rc = ::libssh2_session_handshake(this->ssh_session, this->sock); rc)
        return ssh2_translate_error(rc, nullptr);

    if (auto rc = ::libssh2_userauth_password(this->ssh_session, username.data(), password.data()); rc)
        return ssh2_translate_error(rc, nullptr);

    this->sftp_session = ::libssh2_sftp_init(this->ssh_session);
    if (!this->sftp_session)
        return ssh2_translate_error(::libssh2_session_last_errno(this->ssh_session), this->sftp_session);

    ::libssh2_session_set_blocking(this->ssh_session, 1);

    if (!share.empty())
        this->cwd = share;

    this->is_connected = true;

    return 0;
}

int SftpFs::disconnect() {
    int rc = 0;

    auto lk = std::scoped_lock(this->session_mutex);

    if (this->sftp_session)
        rc |= ::libssh2_sftp_shutdown(this->sftp_session);

    if (this->ssh_session)
        rc |= ::libssh2_session_disconnect(this->ssh_session, "Normal Shutdown");

    if (this->sock > 0)
        rc |= ::close(this->sock);

    this->is_connected = false;

    return rc;
}

std::string SftpFs::translate_path(const char *path) {
    return this->cwd + (path + this->mount_name.length());
}

int SftpFs::sftp_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode) {
    auto *priv      = static_cast<SftpFs     *>(r->deviceData);
    auto *priv_file = static_cast<SftpFsFile *>(fileStruct);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    priv_file->handle = ::libssh2_sftp_open_ex(priv->sftp_session, internal_path.c_str(), internal_path.length(),
        ssh2_translate_open_flags(flags), 0, LIBSSH2_SFTP_OPENFILE);
    if (!priv_file->handle) {
        __errno_r(r) = ssh2_translate_error(::libssh2_session_last_errno(priv->ssh_session), priv->sftp_session);
        return -1;
    }

    auto rc = ::libssh2_sftp_fstat(priv_file->handle, &priv_file->attrs);
    if (rc) {
        __errno_r(r) = ssh2_translate_error(rc, priv->sftp_session);
        return -1;
    }

    priv_file->offset = 0;

    return 0;
}

int SftpFs::sftp_close(struct _reent *r, void *fd) {
    auto *priv      = static_cast<SftpFs     *>(r->deviceData);
    auto *priv_file = static_cast<SftpFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    auto rc = ::libssh2_sftp_close(priv_file->handle);
    if (rc) {
        __errno_r(r) = ssh2_translate_error(rc, priv->sftp_session);
        return -1;
    }

    return 0;
}

ssize_t SftpFs::sftp_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    auto *priv      = static_cast<SftpFs     *>(r->deviceData);
    auto *priv_file = static_cast<SftpFsFile *>(fd);

    auto lk = std::scoped_lock(priv->session_mutex);

    auto rc = ::libssh2_sftp_read(priv_file->handle, ptr, len);
    if (rc < 0) {
        __errno_r(r) = ssh2_translate_error(rc, priv->sftp_session);
        return -1;
    }

    priv_file->offset += rc;

    return rc;
}

off_t SftpFs::sftp_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    auto *priv      = static_cast<SftpFs     *>(r->deviceData);
    auto *priv_file = static_cast<SftpFsFile *>(fd);

    off_t offset;
    switch (dir) {
        default:
        case SEEK_SET:
            offset = 0;
            break;
        case SEEK_CUR:
            offset = priv_file->offset;
            break;
        case SEEK_END:
            offset = priv_file->attrs.filesize;
            break;
    }

    priv_file->offset = offset + pos;

    auto lk = std::scoped_lock(priv->session_mutex);

    ::libssh2_sftp_seek64(priv_file->handle, priv_file->offset);
    return priv_file->offset;
}

int SftpFs::sftp_fstat(struct _reent *r, void *fd, struct stat *st) {
    auto *priv_file = static_cast<SftpFsFile *>(fd);

    ssh2_translate_stat(priv_file->attrs, st);
    return 0;
}

int SftpFs::sftp_stat(struct _reent *r, const char *file, struct stat *st) {
    auto *priv = static_cast<SftpFs *>(r->deviceData);

    auto internal_path = priv->translate_path(file);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    auto rc = ::libssh2_sftp_stat(priv->sftp_session, internal_path.c_str(), &attrs);
    if (rc) {
        __errno_r(r) = ssh2_translate_error(rc, priv->sftp_session);
        return -1;
    }

    ssh2_translate_stat(attrs, st);
    return 0;
}

int SftpFs::sftp_lstat(struct _reent *r, const char *file, struct stat *st) {
    auto *priv = static_cast<SftpFs *>(r->deviceData);

    auto internal_path = priv->translate_path(file);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    auto rc = ::libssh2_sftp_lstat(priv->sftp_session, internal_path.c_str(), &attrs);
    if (rc) {
        __errno_r(r) = ssh2_translate_error(rc, priv->sftp_session);
        return -1;
    }

    ssh2_translate_stat(attrs, st);
    return 0;
}

int SftpFs::sftp_chdir(struct _reent *r, const char *name) {
    auto *priv = static_cast<SftpFs *>(r->deviceData);

    if (!name) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    priv->cwd = name;

    return 0;
}

DIR_ITER *SftpFs::sftp_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    auto *priv     = static_cast<SftpFs    *>(r->deviceData);
    auto *priv_dir = static_cast<SftpFsDir *>(dirState->dirStruct);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return nullptr;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    priv_dir->handle = ::libssh2_sftp_open_ex(priv->sftp_session, internal_path.c_str(), internal_path.length(),
        0, 0, LIBSSH2_SFTP_OPENDIR);
    if (!priv_dir->handle) {
        __errno_r(r) = ssh2_translate_error(::libssh2_session_last_errno(priv->ssh_session), priv->sftp_session);
        return nullptr;
    }

    return dirState;
}

int SftpFs::sftp_dirreset(struct _reent *r, DIR_ITER *dirState) {
    __errno_r(r) = ENOSYS;
    return -1;
}

int SftpFs::sftp_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto *priv     = static_cast<SftpFs    *>(r->deviceData);
    auto *priv_dir = static_cast<SftpFsDir *>(dirState->dirStruct);

    auto lk = std::scoped_lock(priv->session_mutex);

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (true) {
        auto rc = ::libssh2_sftp_readdir(priv_dir->handle, filename, NAME_MAX, &attrs);
        if (rc == 0) {
            __errno_r(r) = ENOENT;
            return -1;
        } else if (rc < 0) {
            __errno_r(r) = ssh2_translate_error(rc, priv->sftp_session);
            return -1;
        }

        auto fname = std::string_view(filename);
        if (fname != "." && fname != "..")
            break;
    }

    ssh2_translate_stat(attrs, filestat);
    return 0;
}

int SftpFs::sftp_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto *priv     = static_cast<SftpFs    *>(r->deviceData);
    auto *priv_dir = static_cast<SftpFsDir *>(dirState->dirStruct);

    auto lk = std::scoped_lock(priv->session_mutex);

    return ssh2_translate_error(::libssh2_sftp_closedir(priv_dir->handle), priv->sftp_session);
}

int SftpFs::sftp_statvfs(struct _reent *r, const char *path, struct statvfs *buf) {
    auto *priv = static_cast<SftpFs *>(r->deviceData);

    auto internal_path = priv->translate_path(path);
    if (internal_path.empty()) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    auto lk = std::scoped_lock(priv->session_mutex);

    LIBSSH2_SFTP_STATVFS st;
    auto rc = ::libssh2_sftp_statvfs(priv->sftp_session, internal_path.c_str(), internal_path.length(), &st);
    if (rc) {
        __errno_r(r) = ssh2_translate_error(rc, priv->sftp_session);
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
