#include "AppFramework/Editor/EditorSelection.h"

namespace SasamiEngine::App
{
    const char* EditorObjectKindName(EditorObjectKind kind)
    {
        switch (kind)
        {
        case EditorObjectKind::StaticModel: return "static";
        case EditorObjectKind::SkinnedModel: return "skinned";
        case EditorObjectKind::PointLight: return "point";
        case EditorObjectKind::SpotLight: return "spot";
        case EditorObjectKind::Camera: return "camera";
        case EditorObjectKind::DirectionalLight: return "directional";
        default: return "none";
        }
    }
}
