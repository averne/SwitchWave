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
#include <new>
#include <string>
#include <string_view>
#include <sys/syslimits.h>

#include <curl/curl.h>

#include "fs/fs_http.hpp"

namespace sw::fs {

namespace {

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

void parse_autoindex(std::string_view html, std::vector<HttpFs::DirEntry> &entries) {
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

        .structSize   = 0,
        .open_r       = HttpFs::http_open,
        .close_r      = HttpFs::http_close,
        .read_r       = HttpFs::http_read,
        .seek_r       = HttpFs::http_seek,
        .fstat_r      = HttpFs::http_fstat,

        .stat_r       = HttpFs::http_stat,

        .dirStateSize = sizeof(HttpFsDir),
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
    if (auto rc = ::curl_global_init(CURL_GLOBAL_DEFAULT); rc)
        return EIO;

    return 0;
}

int HttpFs::connect(std::string_view host, std::uint16_t port, std::string_view share,
        std::string_view username, std::string_view password) {
    bool is_https = (this->protocol == Protocol::Https);
    auto scheme = is_https ? "https://" : "http://";
    auto default_port = is_https ? std::uint16_t(443) : std::uint16_t(80);

    // Build host[:port] string
    auto host_port = std::string(host);
    if (port && port != default_port) {
        host_port += ':';
        host_port += std::to_string(port);
    }

    while (!share.empty() && share.front() == '/')
        share.remove_prefix(1);
    while (!share.empty() && share.back() == '/')
        share.remove_suffix(1);

    // Build base URL: http(s)://host:port[/share]
    auto base = Path(std::string(scheme) + host_port);
    if (!share.empty())
        base /= Path(std::string(share));
    this->base_url = base.base();

    // Store credentials for curl operations
    this->userpwd.clear();
    if (!username.empty()) {
        this->userpwd = username;
        this->userpwd += ':';
        this->userpwd += password;
    }

    // Build auth URL prefix with embedded credentials for make_url()
    std::string auth_authority = scheme;
    if (!username.empty()) {
        auth_authority += username;
        auth_authority += ':';
        auth_authority += password;
        auth_authority += '@';
    }
    auth_authority += host_port;

    auto auth_base = Path(std::move(auth_authority));
    if (!share.empty())
        auth_base /= Path(std::string(share));
    this->auth_url_prefix = auth_base.base();

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

    this->is_connected = false;

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
    auto internal = Path::internal(path);
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
    auto url = priv->base_url + url_encode_path(internal_path);

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

    int ret = 0;
    switch (http_code) {
        case 200: {
            curl_off_t cl = -1;
            ::curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
            st->st_size = (cl >= 0) ? cl : 0;
            st->st_mode = S_IFREG;
            break;
        }
        case 301:
        case 302:
            st->st_mode = S_IFDIR;
            break;
        case 404:
            __errno_r(r) = ENOENT, ret = -1;
            break;
        case 403:
            __errno_r(r) = EACCES, ret = -1;
            break;
        default:
            __errno_r(r) = EIO, ret = -1;
            break;
    }

    ::curl_easy_cleanup(curl);
    return ret;
}

int HttpFs::http_lstat(struct _reent *r, const char *file, struct stat *st) {
    return HttpFs::http_stat(r, file, st);
}

DIR_ITER *HttpFs::http_diropen(struct _reent *r, DIR_ITER *dirState, const char *path) {
    auto *priv     = static_cast<HttpFs    *>(r->deviceData);
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);

    priv_dir->entries = nullptr;
    priv_dir->index = 0;

    auto internal_path = priv->translate_path(path);
    auto url = priv->base_url + url_encode_path(internal_path);
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

    auto *entries = new(std::nothrow) std::vector<DirEntry>;
    if (!entries) {
        __errno_r(r) = ENOMEM;
        return nullptr;
    }

    parse_autoindex(html, *entries);

    priv_dir->entries = entries;
    priv_dir->index = 0;

    return dirState;
}

int HttpFs::http_dirreset(struct _reent *r, DIR_ITER *dirState) {
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);

    if (!priv_dir->entries) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    priv_dir->index = 0;

    return 0;
}

int HttpFs::http_dirnext(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *filestat) {
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);
    auto *entries = priv_dir->entries;

    if (!entries) {
        __errno_r(r) = EINVAL;
        return -1;
    }

    if (priv_dir->index >= entries->size()) {
        __errno_r(r) = ENOENT;
        return -1;
    }

    auto &entry = (*entries)[priv_dir->index++];
    std::strncpy(filename, entry.href.c_str(), NAME_MAX);

    *filestat = {};
    filestat->st_mode = entry.is_dir ? S_IFDIR : S_IFREG;

    return 0;
}

int HttpFs::http_dirclose(struct _reent *r, DIR_ITER *dirState) {
    auto *priv_dir = static_cast<HttpFsDir *>(dirState->dirStruct);

    delete priv_dir->entries;
    priv_dir->entries = nullptr;
    priv_dir->index = 0;

    return 0;
}

} // namespace sw::fs
