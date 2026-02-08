#pragma once
// Ensure C++20 library facilities like std::construct_at are declared
// before any STL headers that may use them internally.
#include <memory>
#include <new>
#include <utility>

// Polyfill for std::construct_at if missing in the STL used.
// Guarded by the C++ feature-test macro when available.
#ifndef __cpp_lib_construct_at
namespace std {
    template <class T, class... Args>
    constexpr T* construct_at(T* p, Args&&... args)
        noexcept(noexcept(::new (const_cast<void*>(static_cast<const volatile void*>(p))) T(std::forward<Args>(args)...)))
    {
        return ::new (const_cast<void*>(static_cast<const volatile void*>(p))) T(std::forward<Args>(args)...);
    }
}
#endif
