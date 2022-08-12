#pragma once

namespace ampnx::utils {

#define _AMPNX_CAT(x, y) x ## y
#define  AMPNX_CAT(x, y) _AMPNX_CAT(x, y)
#define _AMPNX_STR(x) #x
#define  AMPNX_STR(x) _AMPNX_STR(x)

#define AMPNX_ANONYMOUS AMPNX_CAT(var, __COUNTER__)

#define AMPNX_SCOPEGUARD(f) auto AMPNX_ANONYMOUS = ::ampnx::utils::ScopeGuard(f)

#define AMPNX_UNUSED(...) ::ampnx::utils::variadic_unused(__VA_ARGS__)

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

template <typename F>
struct ScopeGuard {
    [[nodiscard]] ScopeGuard(F &&f): f(std::move(f)) { }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator =(const ScopeGuard &) = delete;

    ~ScopeGuard() {
        this->f();
    }

    private:
        F f;
};

} // namespace ampnx::utils
