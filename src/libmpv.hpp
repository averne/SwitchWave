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

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <mpv/client.h>

#include "context.hpp"

namespace sw {

#define MPV_CALL(expr) ({               \
    if (auto _err = (expr); _err < 0)   \
        return _err;                    \
})

class LibmpvController {
    public:
        using LogCallback        = void(*)(void *user, mpv_event_log_message *mes);
        using FileLoadedCallback = void(*)(void *user);
        using EndFileCallback    = void(*)(void *user, mpv_event_end_file *end);
        using IdleCallback       = void(*)(void *user);
        using PropertyCallback   = void(*)(void *user, mpv_event_property *prop);

    public:
        constexpr static std::string_view MpvDirectory = Context::AppDirectory;

    public:
        ~LibmpvController();

        int initialize();

        void process_events();

        void set_log_callback(LogCallback callback, void *user = nullptr) {
            this->log_callback = callback, this->log_callback_user = user;
        }

        void set_file_loaded_callback(FileLoadedCallback callback, void *user = nullptr) {
            this->file_loaded_callback = callback, this->file_loaded_callback_user = user;
        }

        void set_end_file_callback(EndFileCallback callback, void *user = nullptr) {
            this->end_file_callback = callback, this->end_file_callback_user = user;
        }

        void set_idle_callback(IdleCallback callback, void *user = nullptr) {
            this->idle_callback = callback, this->idle_callback_user = user;
        }

        template <typename... Args>
        int command(Args ...args) {
            const char *cmd[sizeof...(Args)+1] = {args..., nullptr};
            return mpv_command(this->mpv, cmd);
        }

        template <typename... Args>
        int command_async(Args ...args) {
            const char *cmd[sizeof...(Args)+1] = {args..., nullptr};
            return mpv_command_async(this->mpv, 0, cmd);
        }

        template <typename T>
        int get_property(std::string_view name, T &res) {
            return this->get_property(name, LibmpvController::to_mpv_format<T>(), &res);
        }

        int get_property(std::string_view name, mpv_format fmt, void *data) {
            return mpv_get_property(this->mpv, name.data(), fmt, data);
        }

        template <typename T>
        int get_property_async(std::string_view name, T *res = nullptr, PropertyCallback callback = nullptr, void *user = nullptr) {
            return this->get_property_async(name, LibmpvController::to_mpv_format<T>(), res, callback);
        }

        int get_property_async(std::string_view name, mpv_format fmt, void *data = nullptr,
            PropertyCallback callback = nullptr, void *user = nullptr);

        template <typename T>
        int set_property(std::string_view name, T val) {
            return this->set_property(name, LibmpvController::to_mpv_format<T>(), &val);
        }

        int set_property(std::string_view name, mpv_format fmt, void *val) {
            return mpv_set_property(this->mpv, name.data(), fmt, val);
        }

        template <typename T>
        int set_property_async(std::string_view name, T val) {
            return this->set_property_async(name, LibmpvController::to_mpv_format<T>(), &val);
        }

        int set_property_async(std::string_view name, mpv_format fmt, void *val) {
            return mpv_set_property_async(this->mpv, 0, name.data(), fmt, val);
        }

        template <typename T>
        int observe_property(std::string_view name, T *res = nullptr, PropertyCallback callback = nullptr, void *user = nullptr) {
            return this->observe_property(name, LibmpvController::to_mpv_format<T>(), res, callback, user);
        }

        int observe_property(std::string_view name, mpv_format fmt, void *data = nullptr,
            PropertyCallback callback = nullptr, void *user = nullptr);

        int unobserve_property(std::string_view name);

        inline auto *get_handle() {
            return this->mpv;
        }

        template <typename T>
        static T node_map_find(mpv_node_list *l, std::string_view s) {
            for (int i = 0; i < l->num; ++i) {
                if (s == l->keys[i])
                    return *reinterpret_cast<T *>(&l->values[i].u);
            }
            return {};
        };

        template <typename T>
        constexpr static mpv_format to_mpv_format() {
            using Decayed = std::decay_t<T>;
            if constexpr (std::is_same_v<Decayed, int>)
                return MPV_FORMAT_FLAG;
            else if constexpr (std::is_same_v<Decayed, std::int64_t>)
                return MPV_FORMAT_INT64;
            else if constexpr (std::is_same_v<Decayed, double>)
                return MPV_FORMAT_DOUBLE;
            else if constexpr (std::is_same_v<Decayed, const char *> || std::is_same_v<Decayed, char *>)
                return MPV_FORMAT_STRING;
            else if constexpr (std::is_same_v<Decayed, mpv_node>)
                return MPV_FORMAT_NODE;
            else
                return MPV_FORMAT_NONE;
        }

    private:
        struct TrackedProperty {
            mpv_format       format;
            void            *data;
            PropertyCallback callback;
            void            *callback_user;
        };

    private:
        mpv_handle *mpv;

        LogCallback log_callback                = nullptr;
        FileLoadedCallback file_loaded_callback = nullptr;
        EndFileCallback end_file_callback       = nullptr;
        IdleCallback idle_callback              = nullptr;
        void *log_callback_user                 = nullptr;
        void *file_loaded_callback_user         = nullptr;
        void *end_file_callback_user            = nullptr;
        void *idle_callback_user                = nullptr;

        // TODO: Try using std::list for these and pass ptrs to reply_userdata
        // TODO: Track set_property_async replies?
        std::unordered_map<std::string_view, TrackedProperty> async_properties;
        std::unordered_map<std::string_view, TrackedProperty> observed_properties;
};

} // namespace sw
