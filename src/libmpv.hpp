#pragma once

#include <cstdint>
#include <type_traits>

#include <mpv/client.h>

namespace ampnx {

#define MPV_CALL(expr) ({               \
    if (auto _err = (expr); _err < 0)   \
        return _err;                    \
})


class LibmpvController {
    public:
        constexpr static auto MpvDirectory = "/switch/AmpNX";

    public:
        ~LibmpvController();

        int initialize();

        int load_file(const char *path);

        int tell(std::int64_t &seconds);
        int seek(std::int64_t seconds, bool relative = true);

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
        int get_property(const char *name, T &res) {
            return get_property(mpv_get_property, name, res);
        }

        template <typename T>
        int get_property_async(const char *name) {
            auto handler = [](auto mpv, auto name, auto fmt, [[maybe_unused]] auto res) {
                return mpv_get_property_async(mpv, 0, name, fmt);
            };
            T dummy;
            return get_property(handler, name, dummy);
        }

        inline auto *get_handle() {
            return this->mpv;
        }

    private:
        template <typename T>
        int get_property(auto func, const char *name, T &res) {
            if constexpr (std::is_same_v<T, std::int64_t>)
                return func(this->mpv, name, MPV_FORMAT_INT64, &res);
            else if constexpr (std::is_same_v<T, double>)
                return func(this->mpv, name, MPV_FORMAT_DOUBLE, &res);
            else if constexpr (std::is_same_v<T, mpv_node>)
                return func(this->mpv, name, MPV_FORMAT_NODE, &res);
            else
                return 0;
        }

    private:
        mpv_handle *mpv;
};

} // namespace ampnx
