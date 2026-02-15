#pragma once

#include <concepts>

namespace SasamiRenderer
{
    class IComponent
    {
    public:
        virtual ~IComponent() = default;
    };

    template<typename T>
    concept ComponentType = std::derived_from<T, IComponent>;
}
