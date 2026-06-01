#include "Renderer/Scene/DrawCommandBuilder.h"
#include "Renderer/Scene/Material.h"

namespace SasamiRenderer
{
    void DrawCommandBuilder::RecordAll(IRhiCommandEncoder* enc, MeshBuffer& buffer) const
    {
        const auto& items = buffer.Items();
        for (size_t i = 0; i < items.size(); ++i) {
            buffer.Bind(enc, i);
            const auto& it = items[i];
            if (it.indexCount > 0) {
                enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
            } else if (it.vertexCount > 0) {
                enc->Draw({ it.vertexCount, 1, 0, 0 });
            }
        }
    }

    void DrawCommandBuilder::RecordTextured(IRhiCommandEncoder* enc,
                                            MeshBuffer& buffer,
                                            size_t itemIndex,
                                            const Texture& tex,
                                            UINT rootParamIndex) const
    {
        enc->SetGraphicsDescriptorTable(rootParamIndex, { tex.srv.ptr });

        buffer.Bind(enc, itemIndex);
        const auto& items = buffer.Items();
        if (itemIndex < items.size()) {
            const auto& it = items[itemIndex];
            if (it.indexCount > 0) {
                enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
            } else if (it.vertexCount > 0) {
                enc->Draw({ it.vertexCount, 1, 0, 0 });
            }
        }
    }

    void DrawCommandBuilder::RecordAllTextured(IRhiCommandEncoder* enc,
                                               MeshBuffer& buffer,
                                               const Texture& tex,
                                               UINT rootParamIndex) const
    {
        enc->SetGraphicsDescriptorTable(rootParamIndex, { tex.srv.ptr });
        RecordAll(enc, buffer);
    }

    void DrawCommandBuilder::RecordSkinnedTextured(IRhiCommandEncoder* enc,
                                                   SkinnedMeshBuffer& buffer,
                                                   size_t itemIndex,
                                                   const Texture& tex,
                                                   RhiGpuAddress boneMatricesCbGpu,
                                                   UINT texRootParamIndex,
                                                   UINT boneCbRootParamIndex) const
    {
        enc->SetGraphicsDescriptorTable(texRootParamIndex, { tex.srv.ptr });
        if (boneMatricesCbGpu != 0) {
            enc->SetGraphicsConstantBufferView(boneCbRootParamIndex, boneMatricesCbGpu);
        }

        buffer.Bind(enc, itemIndex);
        const auto& items = buffer.Items();
        if (itemIndex < items.size()) {
            const auto& it = items[itemIndex];
            if (it.indexCount > 0) {
                enc->DrawIndexed({ it.indexCount, 1, 0, 0, 0 });
            } else if (it.vertexCount > 0) {
                enc->Draw({ it.vertexCount, 1, 0, 0 });
            }
        }
    }
}
