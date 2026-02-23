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
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <sys/syslimits.h>

#include <curl/curl.h>

#include "fs/fs_http.hpp"

namespace sw::fs {

namespace {

struct DirEntry {
    std::string href;
    bool is_dir;
};

struct DirData {
    std::vector<DirEntry> entries;
    std::size_t index = 0;
};

std::size_t string_write_cb(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *str = static_cast<std::string *>(userdata);
    auto total = size * nmemb;
    str->append(ptr, total);
    return total;
}

std::string url_decode(std::string_view s) {
    std::string result;
    result.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = { s[i+1], s[i+2], '\0' };
            char *end;
            auto val = std::strtoul(hex, &end, 16);
            if (end == hex + 2) {
                result += static_cast<char>(val);
                i += 2;
                continue;
            }
        }
        result += s[i];
    }

    return result;
}

std::string url_encode_path(std::string_view s) {
    std::string result;
    result.reserve(s.size());

    for (auto c: s) {
        if (c == '/' || std::isalnum(static_cast<unsigned char>(c)) ||
                c == '-' || c == '_' || c == '.' || c == '~')
            result += c;
        else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            result += buf;
        }
    }

    return result;
}

void parse_autoindex(std::string_view html, std::vector<DirEntry> &entries) {
    // Narrow search to <table> or <body> if present
    auto body = html;
    if (auto pos = html.find("<table"); pos != std::string_view::npos)
        body = html.substr(pos);
    else if (auto pos2 = html.find("<body"); pos2 != std::string_view::npos)
        body = html.substr(pos2);

    // Iterate over <a href="..."> tags
    std::string_view needle = "href=\"";
    std::size_t search_pos = 0;

    while (search_pos < body.size()) {
        auto href_start = body.find(needle, search_pos);
        if (href_start == std::string_view::npos)
            break;

        href_start += needle.size();
        auto href_end = body.find('"', href_start);
        if (href_end == std::string_view::npos)
            break;

        search_pos = href_end + 1;

        auto href = body.substr(href_start, href_end - href_start);

        // Skip parent directory links
        if (href == "../" || href == "..")
            continue;

        // Skip external links (absolute URLs)
        if (href.find("://") != std::string_view::npos)
            continue;

        // Skip query/anchor-only links
        if (!href.empty() && (href[0] == '?' || href[0] == '#'))
            continue;

        // Skip absolute paths that go to parent
        if (!href.empty() && href[0] == '/')
            continue;

        auto decoded = url_decode(href);
        bool is_dir = !decoded.empty() && decoded.back() == '/';

        // Remove trailing slash for directory names
        if (is_dir && decoded.size() > 1)
            decoded.pop_back();

        if (!decoded.empty())
            entries.push_back({ std::move(decoded), is_dir });
    }
}

} // namespace

HttpFs::HttpFs(Context &context, std::string_view name, std::string_view mount_name): context(context) {
    this->type       = Filesystem::Type::Network;
    this->name       = name;
    this->mount_name = mount_name;

    this->devoptab = {
        .name         = this->name.data(),

        .structSize   = sizeof(HttpFs),
        .open_r       = HttpFs::http_open,
        .close_r      = HttpFs::http_close,
        .read_r       = HttpFs::http_read,
        .seek_r       = HttpFs::http_seek,
        .fstat_r      = HttpFs::http_fstat,

        .stat_r       = HttpFs::http_stat,

        .dirStateSize = sizeof(HttpFs),
        .diropen_r    = HttpFs::http_diropen,
        .dirreset_r   = HttpFs::http_dirreset,
        .dirnext_r    = HttpFs::http_dirnext,
        .dirclose_r   = HttpFs::http_dirclose,

        .deviceData   = this,

        .lstat_r      = HttpFs::http_lstat,
    };
}

HttpFs::~HttpFs() {
    if (this->is_connected)
        this->disconnect();

    this->unregister_fs();
}

int HttpFs::initialize() {
    if (HttpFs::lib_refcount++ == 0) {
        if (auto rc = ::curl_global_init(CURL_GLOBAL_DEFAULT); rc)
            return EIO;
    }

    return 0;
}

int HttpFs::connect(std::string_view host, std::uint16_t port, std::string_view share,
        std::string_view username, std::string_view password) {
    bool is_https = (this->protocol == Protocol::Https);
    auto scheme = is_https ? "https://" : "http://";
    auto default_port = is_https ? std::uint16_t(443) : std::uint16_t(80);

    // Build base URL: http(s)://host:port/share/
    this->base_url = scheme;
    this->base_url += host;
    if (port && port != default_port) {
        this->base_url += ':';
        this->base_url += std::to_string(port);
    }
    this->base_url += '/';
    if (!share.empty()) {
        if (share.front() == '/')
            share = share.substr(1);
        this->base_url += share;
        if (this->base_url.back() != '/')
            this->base_url += '/';
    }

    // Store credentials for curl operations
    this->userpwd.clear();
    if (!username.empty()) {
        this->userpwd = username;
        this->userpwd += ':';
        this->userpwd += password;
    }

    // Build auth URL prefix with embedded credentials for make_url()
    this->auth_url_prefix = scheme;
    if (!username.empty()) {
        this->auth_url_prefix += username;
        this->auth_url_prefix += ':';
        this->auth_url_prefix += password;
        this->auth_url_prefix += '@';
    }
    this->auth_url_prefix += host;
    if (port && port != default_port) {
        this->auth_url_prefix += ':';
        this->auth_url_prefix += std::to_string(port);
    }
    this->auth_url_prefix += '/';
    if (!share.empty()) {
        this->auth_url_prefix += share;
        if (this->auth_url_prefix.back() != '/')
            this->auth_url_prefix += '/';
    }

    // Test connection with HEAD request
    auto *curl = ::curl_easy_init();
    if (!curl)
        return ENOMEM;

    this->setup_curl_handle(curl);
    ::curl_easy_setopt(curl, CURLOPT_URL, this->base_url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

    auto res = ::curl_easy_perform(curl);
    ::curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::printf("HTTP connect failed: %s\n", ::curl_easy_strerror(res));
        return ECONNREFUSED;
    }

    auto lk = std::scoped_lock(this->session_mutex);
    this->is_connected = true;

    return 0;
}

int HttpFs::disconnect() {
    auto lk = std::scoped_lock(this->session_mutex);

    this->base_url.clear();
    this->userpwd.clear();
    this->auth_url_prefix.clear();
    this->is_connected = false;

    if (--HttpFs::lib_refcount == 0)
        ::curl_global_cleanup();

    return 0;
}

void HttpFs::setup_curl_handle(void *handle) {
    auto *curl = static_cast<CURL *>(handle);

    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    ::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    ::curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchWave/1.0");

    if (!this->userpwd.empty()) {
        ::curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        ::curl_easy_setopt(curl, CURLOPT_USERPWD, this->userpwd.c_str());
    }
}

std::string HttpFs::translate_path(const char *path) {
    return this->cwd + (path + this->mount_name.length());
}

std::string HttpFs::make_url(std::string_view path) const {
    // path is like "mountname:/some/dir/file.mkv", strip mountpoint
    auto internal = Path::internal(path);
    // Skip leading slash
    if (!internal.empty() && internal.front() == '/')
        internal = internal.substr(1);

    return this->auth_url_prefix + url_encode_path(internal);
}

// File operations: not supported, files are accessed via direct HTTP URL
int HttpFs::http_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode) {
    __errno_r(r) = ENOSYS;
    return -1;
}

int HttpFs::http_close(struct _reent *r, void *fd) {
    __errno_r(r) = ENOSYS;
    return -1;
}

ssize_t HttpFs::http_read(struct _reent *r, void *fd, char *ptr, size_t len) {
    __errno_r(r) = ENOSYS;
    return -1;
}

off_t HttpFs::http_seek(struct _reent *r, void *fd, off_t pos, int dir) {
    __errno_r(r) = ENOSYS;
    return -1;
}

int HttpFs::http_fstat(struct _reent *r, void *fd, struct stat *st) {
    __errno_r(r) = ENOSYS;
    return -1;
}

int HttpFs::http_stat(struct _reent *r, const char *file, struct stat *st) {
    auto *priv = static_cast<HttpFs *>(r->deviceData);

    auto internal_path = priv->translate_path(file);
    // Build full URL for stat
    auto url = priv->base_url + url_encode_path(std::string_view(internal_path).substr(1));

    auto lk = std::scoped_lock(priv->session_mutex);

    auto *curl = ::curl_easy_init();
    if (!curl) {
        __errno_r(r) = ENOMEM;
        return -1;
    }

    priv->setup_curl_handle(curl);
    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    auto res = ::curl_easy_perform(curl);
    if (res != CURLE_OK) {
        ::curl_easy_cleanup(curl);
        __errno_r(r) = ENOENT;
        return -1;
    }

    long http_code = 0;
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    *st = {};

    if (http_code == 200) {
        // Check content-length
        curl_off_t cl = -1;
        ::curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        st->st_size = (cl >= 0) ? cl : 0;
        st->st_mode = S_IFREG;
    } else if (http_code == 301 || http_code == 302) {
        st->st_mode = S_IFDIR;
    } else if (http_code == 404) {
        ::curl_easy_cleanup(curl);
        __errno_r(r) = ENOENT;
        return -1;
    } else if (http_code == 403) {
        ::curl_easy_cleanup(curl);
        __errno_r(r) = EACCES;
        return -1;
    } else {
        ::curl_easy_cleanup(curl);
        __errno_r(r) = EIO;
        return -1;
    }

    ::curl_easy_cleanup(curl);
    return 0;
}

int HttpFs::http_lstat(struct _reent *r, const char *file, struct stat *st) {
    return HttpFs::http_stat(r, file, st);
}

DIR_ITER *HttpFs::http_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    auto *priv     = static_cast<HttpFs    *>(r->deviceData);
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);

    auto internal_path = priv->translate_path(path);
    auto url = priv->base_url;
    if (internal_path.size() > 1)
        url += url_encode_path(std::string_view(internal_path).substr(1));
    if (url.back() != '/')
        url += '/';

    auto lk = std::scoped_lock(priv->session_mutex);

    auto *curl = ::curl_easy_init();
    if (!curl) {
        __errno_r(r) = ENOMEM;
        return nullptr;
    }

    priv->setup_curl_handle(curl);

    std::string html;
    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, string_write_cb);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &html);

    auto res = ::curl_easy_perform(curl);
    ::curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        __errno_r(r) = EIO;
        return nullptr;
    }

    auto *dir_data = new DirData();
    parse_autoindex(html, dir_data->entries);

    priv_dir->data = dir_data;
    return dirState;
}

int HttpFs::http_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);
    auto *dir_data = static_cast<DirData *>(priv_dir->data);

    if (dir_data)
        dir_data->index = 0;

    return 0;
}

int HttpFs::http_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);
    auto *dir_data = static_cast<DirData *>(priv_dir->data);

    if (!dir_data || dir_data->index >= dir_data->entries.size()) {
        __errno_r(r) = ENOENT;
        return -1;
    }

    auto &entry = dir_data->entries[dir_data->index++];
    std::strncpy(filename, entry.href.c_str(), NAME_MAX);

    *filestat = {};
    filestat->st_mode = entry.is_dir ? S_IFDIR : S_IFREG;

    return 0;
}

int HttpFs::http_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);

    delete static_cast<DirData *>(priv_dir->data);
    priv_dir->data = nullptr;

    return 0;
}

} // namespace sw::fs
