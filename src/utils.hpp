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

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <array>
#include <string_view>
#include <utility>

namespace sw::utils {

using namespace std::string_view_literals;

#define _SW_CAT(x, y) x ## y
#define  SW_CAT(x, y) _SW_CAT(x, y)
#define _SW_STR(x) #x
#define  SW_STR(x) _SW_STR(x)

#define SW_ANONYMOUS SW_CAT(var, __COUNTER__)

#define SW_SCOPEGUARD(f) auto SW_ANONYMOUS = ::sw::utils::ScopeGuard(f)

#define SW_UNUSED(...) ::sw::utils::variadic_unused(__VA_ARGS__)

#define SW_ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))

template <typename ...Args>
__attribute__((always_inline))
static inline void variadic_unused(Args &&...args) {
    (static_cast<void>(args), ...);
}

constexpr auto align_down(auto v, auto a) {
    return v & ~(a - 1);
}

constexpr auto align_up(auto v, auto a) {
    return align_down(v + a - 1, a);
}

constexpr auto bit(auto bit) {
    return static_cast<decltype(bit)>(1) << bit;
}

constexpr auto mask(auto bit) {
    return (static_cast<decltype(bit)>(1) << bit) - 1;
}

template <typename F>
struct ScopeGuard {
    [[nodiscard]] ScopeGuard(F &&f): f(std::move(f)) { }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator =(const ScopeGuard &) = delete;

    ~ScopeGuard() {
        if (this->want_run)
            this->f();
    }

    void cancel() {
        this->want_run = false;
    }

    private:
        bool want_run = true;
        F f;
};

static inline std::pair<double, std::string_view> to_human_size(std::size_t bytes) {
    static std::array suffixes = {
        "B"sv, "kiB"sv, "MiB"sv, "GiB"sv, "TiB"sv, "PiB"sv,
    };

    // __builtin_clzll is undefined for 0
    if (bytes == 0)
        return { 0.0, suffixes[0] };

    auto mag = (63 - __builtin_clzll(bytes)) / 10;
    return { static_cast<double>(bytes) / (1 << mag*10), suffixes[mag] };
}

template <typename T>
static inline int read_whole_file(T &container, const char *path, const char *mode) {
    FILE *fp = std::fopen(path, mode);
    SW_SCOPEGUARD([fp] { std::fclose(fp); });
    if (!fp) {
        std::printf("Failed to open %s\n", path);
        return -1;
    }

    std::fseek(fp, 0, SEEK_END);
    std::size_t fsize = std::ftell(fp);
    std::rewind(fp);

    container.resize(fsize);

    if (auto read = std::fread(container.data(), 1, container.size(), fp); read != fsize) {
        std::printf("Failed to read %s: got %ld, expected %ld\n", path, read, fsize);
        return -1;
    }

    return 0;
}

template <std::size_t Size>
class StaticString {
    public:
        constexpr inline StaticString() = default;

        constexpr inline StaticString(const char *data) {
            std::strncpy(this->storage, data, this->capacity());
        }

        constexpr inline StaticString(std::string_view sv) {
            std::strncpy(this->storage, sv.data(), this->capacity());
        }

        template <std::size_t Size2>
        constexpr inline StaticString(const StaticString<Size2> &other) {
            static_assert(Size >= Size2);
            std::strncpy(this->storage, other.storage, this->capacity());
        }

        constexpr inline StaticString &operator=(const char *data) {
            std::strncpy(this->storage, data, this->capacity());
            return *this;
        }

        constexpr inline StaticString &operator=(std::string_view sv) {
            std::strncpy(this->storage, sv.data(), this->capacity());
            return *this;
        }

        template <std::size_t Size2>
        constexpr inline StaticString &operator=(const StaticString<Size2> &other) {
            static_assert(Size >= Size2);
            std::strncpy(this->storage, other.storage, this->capacity());
            return *this;
        }

        constexpr inline StaticString<Size> operator+(const char *data) const {
            StaticString<Size> out = *this;
            std::strncat(out.data(), data, out.capacity());
            return out;
        }

        constexpr inline StaticString<Size> operator+(std::string_view sv) const {
            StaticString<Size> out = *this;
            std::strncat(out.data(), sv.data(), out.capacity());
            return out;
        }

        template <std::size_t Size2>
        constexpr inline StaticString<Size> operator+(const StaticString<Size2> &other) const {
            StaticString<Size> out = *this;
            std::strncat(out.data(), other.c_str(), out.capacity());
            return out;
        }

        template <std::size_t Size2>
        constexpr inline bool operator==(const StaticString<Size2> other) const {
            return std::string_view(*this) == std::string_view(other);
        }

        template <std::size_t Size2>
        constexpr inline auto operator<=>(const StaticString<Size2> other) const {
            return std::string_view(*this) <=> std::string_view(other);
        }

        constexpr inline operator std::string_view() const {
            return std::string_view(this->storage);
        }

        constexpr inline const char *c_str() const {
            return this->storage;
        }

        constexpr inline char *data() {
            return this->storage;
        }

        constexpr inline std::size_t size() const {
            return std::strlen(this->storage);
        }

        constexpr inline std::size_t length() const {
            return std::strlen(this->storage);
        }

        constexpr inline static std::size_t capacity() {
            return Size - 1;
        }

    private:
        char storage[Size] = {};
};

using StaticString8  = StaticString< 8>;
using StaticString16 = StaticString<16>;
using StaticString32 = StaticString<32>;
using StaticString64 = StaticString<64>;

} // namespace sw::utils
