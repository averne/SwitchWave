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

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <sys/iosupport.h>

namespace sw::fs {

class Path {
    public:
        template <typename ...Args>
        constexpr Path(Args &&...args): base_(std::forward<Args>(args)...) { }
        constexpr Path(const Path &) = default;
        constexpr Path(Path &&) = default;

        constexpr Path& operator=(const Path &) = default;
        constexpr Path& operator=(Path &&) = default;

        inline operator std::filesystem::path() const {
            return std::filesystem::path(this->base_);
        }

        inline std::size_t length() const {
            return this->base_.length();
        }

        inline std::size_t size() const {
            return this->base_.size();
        }

        inline bool empty() const {
            return this->base_.empty();
        }

        inline void clear() {
            return this->base_.clear();
        }

        constexpr inline const std::string &base() const {
            return this->base_;
        }

        constexpr inline const char *c_str() const {
            return this->base_.c_str();
        }

        constexpr inline Path operator+(const Path &path) {
            return this->base() + path.base();
        }

        constexpr inline Path &operator+=(const Path &path) {
            this->base_ += path.base();
            return *this;
        }

        constexpr inline Path operator/(const Path &path) {
            return this->base() + ((this->base().back() == '/') ? "" : "/") + path.base();
        }

        constexpr inline Path &operator/=(const Path &path) {
            this->base_ += ((this->base().back() == '/') ? "" : "/") + path.base();
            return *this;
        }

        static constexpr inline std::string_view mountpoint(std::string_view path) {
            auto pos = path.find('/');
            return (pos != std::string::npos) ? path.substr(0, path.find('/')) : "";
        }

        static constexpr inline std::string_view internal(std::string_view path) {
            auto pos = path.find('/');
            return (pos != std::string::npos) ? path.substr(pos) : "";
        }

        static constexpr inline std::string_view parent(std::string_view path) {
            return path.substr(0, std::max(path.find('/') + 1, path.rfind('/')));
        }

        static constexpr inline std::string_view filename(std::string_view path) {
            return path.substr(path.rfind('/') + 1);
        }

        static constexpr inline std::string_view extension(std::string_view path) {
            return path.substr(path.rfind('.') + 1);
        }

        static constexpr inline bool is_root(std::string_view path) {
            return Path::internal(path) == "/";
        }

        constexpr inline std::string_view mountpoint() const { return Path::mountpoint(this->base()); }
        constexpr inline std::string_view internal  () const { return Path::internal  (this->base()); }
        constexpr inline std::string_view parent    () const { return Path::parent    (this->base()); }
        constexpr inline std::string_view filename  () const { return Path::filename  (this->base()); }
        constexpr inline std::string_view extension () const { return Path::extension (this->base()); }
        constexpr inline bool is_root() const { return Path::is_root(this->base()); }

    private:
        std::string base_;
};

struct Node {
    enum class Type {
        Directory,
        File,
    };

    Type type;
    std::string name;

    std::size_t size = 0;
};

class Filesystem {
    public:
        enum Type {
            Recent,
            Sdmc,
            Usb,
            Network,
        };

    public:
        constexpr Filesystem() = default;
        constexpr Filesystem(Type type, std::string_view name, std::string_view mount_name):
            type(type), name(name), mount_name(mount_name) { }
        virtual ~Filesystem() = default;

        int register_fs() const {
            auto id = FindDevice(this->mount_name.data());

            if (id < 0)
                id = AddDevice(&this->devoptab);

            if (id < 0)
                return id;

            return 0;
        }

        int unregister_fs() const {
            return RemoveDevice(this->mount_name.data());
        }

    public:
        Type type;
        std::string_view name, mount_name;

    protected:
        devoptab_t devoptab = {};
};

class NetworkFilesystem: public Filesystem {
    public:
        enum Protocol {
            Smb,
            Nfs,
            Sftp,
            Http,
            Https,
            ProtocolMax,
        };

    public:
        virtual ~NetworkFilesystem() = default;

        virtual int initialize() = 0;
        virtual int connect(std::string_view host, std::uint16_t port, std::string_view share,
            std::string_view username, std::string_view password) = 0;
        virtual int disconnect() = 0;

        bool connected() const {
            return this->is_connected;
        }

        static constexpr std::string_view protocol_name(Protocol p) {
            switch (p) {
                case Protocol::Smb:
                default:
                    return "smb";
                case Protocol::Nfs:
                    return "nfs";
                case Protocol::Sftp:
                    return "sftp";
                case Protocol::Http:
                    return "http";
                case Protocol::Https:
                    return "https";
            }
        }

    public:
        Protocol protocol = Protocol::Smb;

    protected:
        bool is_connected = false;
};

} // namespace sw::fs
