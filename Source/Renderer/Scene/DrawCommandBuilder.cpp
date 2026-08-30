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

}
