#include "Renderer/Scene/DrawCommandBuilder.h"
#include "Renderer/Scene/Material.h"

namespace SasamiRenderer
{
    void DrawCommandBuilder::RecordAll(CommandList* cmdList, MeshBuffer& buffer) const
    {
        const auto& items = buffer.Items();
        for (size_t i = 0; i < items.size(); ++i) {
            buffer.Bind(cmdList, i);
            const auto& it = items[i];
            if (it.indexCount > 0) {
                cmdList->DrawIndexedInstanced(it.indexCount, 1, 0, 0, 0);
            } else if (it.vertexCount > 0) {
                cmdList->DrawInstanced(it.vertexCount, 1, 0, 0);
            }
        }
    }

    void DrawCommandBuilder::RecordTextured(CommandList* cmdList,
                                            MeshBuffer& buffer,
                                            size_t itemIndex,
                                            const Texture& tex,
                                            UINT rootParamIndex) const
    {
        // Bind SRV for provided texture (descriptor heap must be set by caller)
        cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, tex.srv);

        // Bind mesh buffers and draw a single item
        buffer.Bind(cmdList, itemIndex);
        const auto& items = buffer.Items();
        if (itemIndex < items.size()) {
            const auto& it = items[itemIndex];
            if (it.indexCount > 0) {
                cmdList->DrawIndexedInstanced(it.indexCount, 1, 0, 0, 0);
            } else if (it.vertexCount > 0) {
                cmdList->DrawInstanced(it.vertexCount, 1, 0, 0);
            }
        }
    }

    void DrawCommandBuilder::RecordAllTextured(CommandList* cmdList,
                                               MeshBuffer& buffer,
                                               const Texture& tex,
                                               UINT rootParamIndex) const
    {
        // Bind SRV once; all items use the same texture
        cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, tex.srv);

        // Draw everything
        RecordAll(cmdList, buffer);
    }
}
