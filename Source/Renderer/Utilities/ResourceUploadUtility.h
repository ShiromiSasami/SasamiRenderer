#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Utilities/TextureMipChain.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    namespace ResourceUploadUtility
    {
        bool CreateUploadBuffer(IRHIDevice& device,
                                std::uint64_t size,
                                Resource& outResource,
                                void** outMappedPtr);

        // format selects how the sampler interprets the bytes, not their layout: base-colour
        // textures pass R8G8B8A8_UNORM_SRGB so the hardware decodes to linear on read, while
        // normal, metallic-roughness and occlusion maps keep the UNORM default because their
        // values are linear measurements. Both formats are 32bpp with the same footprint, so the
        // upload path below is unaffected by the choice. A resource created here is fully typed,
        // so whatever is passed must also be used for the shader resource view.
        bool CreateTexture2DFromRgba8(IRHIDevice& device,
                                      CommandList* cmdList,
                                      const std::uint8_t* pixels,
                                      UINT width,
                                      UINT height,
                                      Resource& outTexture,
                                      Resource& outUpload,
                                      DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM);

        // Upload an RGBA8 texture together with its full mip chain. The chain is produced by
        // TextureMipChainBuilder::BuildRgba8, whose srgbColor argument must agree with the
        // format passed here -- an sRGB view over a chain box-filtered in stored space
        // reintroduces the very error the decode was added to remove.
        bool CreateTexture2DFromRgba8WithMips(IRHIDevice& device,
                                              CommandList* cmdList,
                                              const TextureMipChain& chain,
                                              Resource& outTexture,
                                              Resource& outUpload,
                                              DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM);

        // Upload RGBA8 face data as a cubemap texture (R8G8B8A8_UNORM). Used for fallback (1x1) textures.
        bool CreateTextureCubeFromRgba8Faces(IRHIDevice& device,
                                             CommandList* cmdList,
                                             const std::vector<std::vector<std::uint8_t>>& facePixels,
                                             UINT width,
                                             UINT height,
                                             Resource& outTexture,
                                             Resource& outUpload);

        // Upload a float-face cubemap (R16G16B16A16_FLOAT) to an existing or newly created GPU texture.
        // If preCreated is non-null the caller already allocated the GPU texture (via RHI); creation is skipped.
        bool CreateTextureCubeFromFloatFacesWithMips(IRHIDevice& device,
                                                     CommandList* cmdList,
                                                     const std::vector<std::vector<float>>& subresourcesRgba,
                                                     UINT baseFaceSize,
                                                     UINT mipLevels,
                                                     Resource& outTexture,
                                                     Resource& outUpload,
                                                     Resource* preCreated = nullptr);

        // Upload float RGBA data as a 2D texture (R16G16B16A16_FLOAT).
        // If preCreated is non-null the caller already allocated the GPU texture (via RHI); creation is skipped.
        bool CreateTexture2DFromFloatRgba(IRHIDevice& device,
                                          CommandList* cmdList,
                                          const std::vector<float>& pixels,
                                          UINT width,
                                          UINT height,
                                          Resource& outTexture,
                                          Resource& outUpload,
                                          Resource* preCreated = nullptr);
    }
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
