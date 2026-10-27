#pragma once

#include <concepts>

namespace SasamiEngine::App
{
    class IComponent
    {
    public:
        virtual ~IComponent() = default;
    };

    template<typename T>
    concept ComponentType = std::derived_from<T, IComponent>;
}
namespace SasamiEngine { using namespace App; }
