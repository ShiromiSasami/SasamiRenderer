#pragma once

#include <cstdint>

namespace SasamiEngine::App
{
    class SObject;

    enum class EditorObjectKind : uint8_t
    {
        None,
        StaticModel,
        SkinnedModel,
        PointLight,
        SpotLight,
        Camera,
        DirectionalLight,  // the app's single value-type light; object stays null
    };

    const char* EditorObjectKindName(EditorObjectKind kind);

    // What the editor panels act on. `object` is only valid while ApplicationCore::GetSceneGeneration()
    // still equals `sceneGeneration` and the object is still listed by ApplicationCore.
    struct EditorSelection
    {
        EditorObjectKind kind = EditorObjectKind::None;
        SObject* object = nullptr;
        uint64_t sceneGeneration = 0;
        uint32_t revision = 0;  // bumped on every change, so panels can drop per-selection caches

        void Select(EditorObjectKind newKind, SObject* newObject, uint64_t generation)
        {
            kind = newKind;
            object = newObject;
            sceneGeneration = generation;
            ++revision;
        }
        void Clear() { Select(EditorObjectKind::None, nullptr, 0); }
        bool IsEmpty() const { return kind == EditorObjectKind::None; }
    };
}
