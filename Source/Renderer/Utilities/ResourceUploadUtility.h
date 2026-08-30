#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Utilities/TextureMipChain.h"

#include <cstdint>
#include <vector>

namespace SasamiRenderer
{
    namespace ResourceUploadUtility
    {
        bool CreateUploadBuffer(IRHIDevice& device,
                                std::uint64_t size,
                                Resource& outResource,
                                void** outMappedPtr);

        bool CreateTexture2DFromRgba8(IRHIDevice& device,
                                      CommandList* cmdList,
                                      const std::uint8_t* pixels,
                                      UINT width,
                                      UINT height,
                                      Resource& outTexture,
                                      Resource& outUpload);

        // Upload an RGBA8 texture together with its full mip chain (R8G8B8A8_UNORM).
        // The chain is produced by TextureMipChainBuilder::BuildRgba8.
        bool CreateTexture2DFromRgba8WithMips(IRHIDevice& device,
                                              CommandList* cmdList,
                                              const TextureMipChain& chain,
                                              Resource& outTexture,
                                              Resource& outUpload);

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
