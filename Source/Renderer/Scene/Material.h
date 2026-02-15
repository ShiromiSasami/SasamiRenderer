#pragma once
#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Structures/Texture.h"

namespace SasamiRenderer
{
    // Minimal material matching current pipeline (t0 only)
    enum class TextureSlot { Albedo = 0 };

    class Material {
    public:
        void Set(TextureSlot slot, Texture* tex) {
            if (slot == TextureSlot::Albedo) albedo_ = tex;
        }

        Texture* Get(TextureSlot slot) const {
            return (slot == TextureSlot::Albedo) ? albedo_ : nullptr;
        }

        bool HasTexture(TextureSlot slot) const { return Get(slot) != nullptr; }

        // Bind SRV for the given root parameter index (descriptor heap must be set by caller)
        void Bind(CommandList* cmdList, UINT rootParamIndex = 0) const {
            if (albedo_) {
                cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, albedo_->srv);
            }
        }

    private:
        Texture* albedo_ = nullptr;
    };
}
