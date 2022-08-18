#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <mpv/client.h>

namespace ampnx {

#define MPV_CALL(expr) ({               \
    if (auto _err = (expr); _err < 0)   \
        return _err;                    \
})


class LibmpvController {
    public:
        using LogCallback      = void(*)(void *user, mpv_event_log_message *mes);
        using PropertyCallback = void(*)(void *user, mpv_event_property *prop);

    public:
        constexpr static auto MpvDirectory = "/switch/AmpNX";

    public:
        ~LibmpvController();

        int initialize();

        void process_events();

        void set_log_callback(LogCallback callback, void *user = nullptr) {
            this->log_callback = callback, this->log_callback_user = user;
        }

        void remove_log_callback() {
            this->log_callback = nullptr,  this->log_callback_user = nullptr;
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
            return this->observe_property(name, LibmpvController::to_mpv_format<T>(), res, callback);
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

        // TODO: Remove this
        int load_file(const char *path);

        int tell(std::int64_t &seconds);
        int seek(std::int64_t seconds, bool relative = true);

    private:
        template <typename T>
        static constexpr mpv_format to_mpv_format() {
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

        LogCallback log_callback = nullptr;
        void *log_callback_user  = nullptr;

        // TODO: Try using std::list for these and pass ptrs to reply_userdata
        // TODO: Track set_property_async replies?
        std::unordered_map<std::string_view, TrackedProperty> async_properties;
        std::unordered_map<std::string_view, TrackedProperty> observed_properties;
};

} // namespace ampnx
