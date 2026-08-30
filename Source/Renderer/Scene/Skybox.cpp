#include "Renderer/Scene/Skybox.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Utilities/RendererMathUtility.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "d3dx12.h"

namespace
{
    struct SkyboxVertex
    {
        float position[3];
    };

    static const SkyboxVertex kSkyboxCubeVertices[] = {
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f, -1.0f,  1.0f } }, { { 1.0f,  1.0f,  1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f,  1.0f,  1.0f } }, { { 1.0f,  1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { -1.0f, -1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { -1.0f,  1.0f, -1.0f } }, { { -1.0f,  1.0f,  1.0f } },
        { { -1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, 1.0f } },
        { { -1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, 1.0f } }, { { -1.0f, 1.0f, 1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f, -1.0f } }, { { -1.0f, -1.0f, -1.0f } },
        { { -1.0f, -1.0f, 1.0f } }, { { -1.0f,  1.0f, 1.0f } }, { { 1.0f,  1.0f, 1.0f } },
        { { -1.0f, -1.0f, 1.0f } }, { { 1.0f,  1.0f, 1.0f } }, { { 1.0f, -1.0f, 1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f,  1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } }, { { -1.0f, -1.0f, -1.0f } },
    };

}

namespace SasamiRenderer
{
    using Math::Mul4x4;

    bool Skybox::Initialize(IRHIDevice& device,
                            const AllocateSrvRangeCallback& allocateSrvRange,
                            DescriptorHeap* srvHeap)
    {
        m_device = &device;

        CpuDescriptorHandle skyboxCpu{};
        GpuDescriptorHandle skyboxGpu{};
        if (!allocateSrvRange || !allocateSrvRange(1, skyboxCpu, skyboxGpu)) {
            DebugLogDialog("Skybox::Initialize: SRV allocation failed for skybox.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_skyboxSrvCpu = skyboxCpu;
        m_skyboxSrv = skyboxGpu;
        m_srvAlloc = allocateSrvRange;

        if (!m_iblSystem.Initialize(device, allocateSrvRange)) {
            DebugLogDialog("Skybox::Initialize: IBL system initialization failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        if (!InitializeGeometry()) {
            DebugLogDialog("Skybox::Initialize: geometry initialization failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // GPU cubemap generation is an optional fast path for UploadHdrSkyboxTexture; failure
        // here is not fatal since the CPU equirect->cube conversion still covers it.
        const std::string configuredShaderModel = ShaderCompilationService::GetConfiguredShaderModel();
        const std::string shaderModel = ShaderCompilationService::ResolveEffectiveShaderModel(device.GetDevice(), configuredShaderModel);
        const std::string computeProfile = "cs_" + shaderModel;
        if (!m_cubemapGenerator.Initialize(device, computeProfile, m_srvAlloc, srvHeap)) {
            DebugLog("Skybox::Initialize: GPU sky cubemap generator initialization failed; falling back to CPU generation.\n");
        }

        return true;
    }

    void Skybox::Shutdown()
    {
        RefreshEnvironmentAssets();
        m_skyboxVB.Reset();
        m_skyboxVBV = {};
        if (m_device && m_skyboxRhiVB.IsValid()) {
            m_device->DestroyRhiResource(m_skyboxRhiVB);
        }
        m_skyboxRhiVB = {};
        m_skyboxVbBinding = {};
        m_device = nullptr;
    }

    void Skybox::SetHdrEquirectData(std::vector<float> pixels, UINT width, UINT height)
    {
        if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3u ||
            width == 0 || height == 0) {
            m_sourceType = SourceType::None;
            m_sourceWidth = 0;
            m_sourceHeight = 0;
            m_sourceHdrRgb.clear();
            m_sourceLdrRgba8.clear();
            m_sourceCubemapFaceRgba8.clear();
            DebugLog("Skybox::SetHdrEquirectData failed: invalid HDR source size.\n");
            return;
        }

        m_sourceType = SourceType::HdrRgbFloat;
        m_sourceWidth = width;
        m_sourceHeight = height;
        m_sourceHdrRgb = std::move(pixels);
        m_sourceLdrRgba8.clear();
        m_sourceCubemapFaceRgba8.clear();
    }

    void Skybox::SetLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height)
    {
        if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u ||
            width == 0 || height == 0) {
            m_sourceType = SourceType::None;
            m_sourceWidth = 0;
            m_sourceHeight = 0;
            m_sourceHdrRgb.clear();
            m_sourceLdrRgba8.clear();
            m_sourceCubemapFaceRgba8.clear();
            DebugLog("Skybox::SetLdrEquirectData failed: invalid LDR source size.\n");
            return;
        }

        m_sourceType = SourceType::LdrRgba8;
        m_sourceWidth = width;
        m_sourceHeight = height;
        m_sourceLdrRgba8 = std::move(pixels);
        m_sourceHdrRgb.clear();
        m_sourceCubemapFaceRgba8.clear();
    }

    void Skybox::AdoptPregeneratedIblData(IblSystem::GeneratedIblData&& data)
    {
        m_iblSystem.AdoptPregeneratedIblData(std::move(data));
    }

    void Skybox::SetLdrCubemapFaceData(std::vector<std::vector<uint8_t>> facePixels, UINT width, UINT height)
    {
        const size_t expectedFaceSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        const bool valid =
            width != 0 &&
            height != 0 &&
            facePixels.size() == 6u &&
            std::all_of(facePixels.begin(), facePixels.end(),
                        [expectedFaceSize](const std::vector<uint8_t>& face) {
                            return face.size() == expectedFaceSize;
                        });
        if (!valid) {
            m_sourceType = SourceType::None;
            m_sourceWidth = 0;
            m_sourceHeight = 0;
            m_sourceHdrRgb.clear();
            m_sourceLdrRgba8.clear();
            m_sourceCubemapFaceRgba8.clear();
            DebugLog("Skybox::SetLdrCubemapFaceData failed: invalid cubemap face source size.\n");
            return;
        }

        m_sourceType = SourceType::LdrCubemapFaces;
        m_sourceWidth = width;
        m_sourceHeight = height;
        m_sourceCubemapFaceRgba8 = std::move(facePixels);
        m_sourceHdrRgb.clear();
        m_sourceLdrRgba8.clear();
    }

    void Skybox::ResetSkyboxResources()
    {
        m_skyboxTexture.Reset();
        m_skyboxTextureUpload.Reset();
        m_skyboxTextureUploaded = false;
        m_skyboxUploadAttempted = false;
        m_skyboxTextureIsHdr = false;
    }

    void Skybox::ResetIblResources()
    {
        m_iblSystem.Reset();
    }

    void Skybox::RefreshEnvironmentAssets()
    {
        ResetSkyboxResources();
        ResetIblResources();

        m_hdrEquirectLoaded = false;
        m_hdrEquirectTried = false;
        m_hdrEquirectWidth = 0;
        m_hdrEquirectHeight = 0;
        m_hdrEquirectPixels.clear();
    }

    void Skybox::SetDirectionalLightMarkerAngularRadius(float radians)
    {
        const float minRadius = 0.001f;
        const float maxRadius = 0.25f;
        if (radians < minRadius) {
            radians = minRadius;
        } else if (radians > maxRadius) {
            radians = maxRadius;
        }

        m_directionalLightMarkerAngularRadius = radians;
        const float expandedRadius = radians * 4.0f;
        const float minimumHaloRadius = radians + 0.02f;
        m_directionalLightMarkerHaloAngularRadius =
            (expandedRadius > minimumHaloRadius) ? expandedRadius : minimumHaloRadius;
    }

    bool Skybox::InitializeGeometry()
    {
        if (!m_device) {
            return false;
        }

        const UINT64 vbBytes = sizeof(kSkyboxCubeVertices);

        if (m_device->GetCapabilities().supportsRhiResourceCreation) {
            RhiBufferDesc vbDesc{};
            vbDesc.sizeInBytes = vbBytes;
            vbDesc.strideInBytes = sizeof(SkyboxVertex);
            vbDesc.usage = RhiBufferUsageFlags::Vertex;
            vbDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
            vbDesc.initialState = RhiResourceState::Common;

            m_skyboxRhiVB = m_device->CreateRhiBuffer(vbDesc, kSkyboxCubeVertices);
            if (!m_skyboxRhiVB.IsValid()) {
                return false;
            }

            m_skyboxVbBinding.buffer = m_skyboxRhiVB;
            m_skyboxVbBinding.offsetInBytes = 0;
            m_skyboxVbBinding.strideInBytes = sizeof(SkyboxVertex);
            m_skyboxVbBinding.sizeInBytes = static_cast<uint32_t>(vbBytes);
            return true;
        }

        if (!m_device->GetCapabilities().supportsD3D12CompatibilitySurface) {
            return false;
        }

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC vbDesc = {};
        vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vbDesc.Width = vbBytes;
        vbDesc.Height = 1;
        vbDesc.DepthOrArraySize = 1;
        vbDesc.MipLevels = 1;
        vbDesc.SampleDesc.Count = 1;
        vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = m_device->CreateCommittedResource(&heapProps,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &vbDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       m_skyboxVB);
        if (FAILED(hr)) {
            return false;
        }

        void* mapped = nullptr;
        hr = m_skyboxVB->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) {
            m_skyboxVB.Reset();
            return false;
        }
        std::memcpy(mapped, kSkyboxCubeVertices, sizeof(kSkyboxCubeVertices));
        m_skyboxVB->Unmap(0, nullptr);

        m_skyboxVBV.BufferLocation = m_skyboxVB->GetGPUVirtualAddress();
        m_skyboxVBV.StrideInBytes = sizeof(SkyboxVertex);
        m_skyboxVBV.SizeInBytes = static_cast<UINT>(sizeof(kSkyboxCubeVertices));
        return true;
    }

    bool Skybox::EnsureHdrEnvironmentLoaded()
    {
        if (m_hdrEquirectLoaded) {
            return true;
        }
        if (m_hdrEquirectTried) {
            return false;
        }
        m_hdrEquirectTried = true;

        if (m_skyboxLoadFormat == SkyboxLoadFormat::CubemapFaces) {
            DebugLog("Skybox::EnsureHdrEnvironmentLoaded: CubemapFaces selected, skip equirect load.\n");
            return false;
        }

        if (m_sourceType == SourceType::None || m_sourceWidth == 0 || m_sourceHeight == 0) {
            DebugLog("Skybox::EnsureHdrEnvironmentLoaded failed: equirect input data is not set.\n");
            return false;
        }

        auto loadFromFloatRgb = [this](std::vector<float> pixels, UINT width, UINT height) -> bool {
            if (pixels.empty() || width == 0 || height == 0) {
                return false;
            }
            m_hdrEquirectWidth = width;
            m_hdrEquirectHeight = height;
            m_hdrEquirectPixels = std::move(pixels);
            m_hdrEquirectLoaded = true;
            return true;
        };

        auto tryLoadHdrFromInput = [&]() -> bool {
            if (m_sourceType != SourceType::HdrRgbFloat) {
                return false;
            }
            if (m_sourceHdrRgb.size() !=
                static_cast<size_t>(m_sourceWidth) * static_cast<size_t>(m_sourceHeight) * 3u) {
                return false;
            }
            if (!loadFromFloatRgb(m_sourceHdrRgb, m_sourceWidth, m_sourceHeight)) {
                return false;
            }
            DebugLog("Loaded HDR equirect environment map for runtime skybox/IBL generation.\n");
            return true;
        };

        auto tryLoadLdrFromInput = [&]() -> bool {
            if (m_sourceType != SourceType::LdrRgba8) {
                return false;
            }
            if (m_sourceLdrRgba8.size() !=
                static_cast<size_t>(m_sourceWidth) * static_cast<size_t>(m_sourceHeight) * 4u) {
                return false;
            }

            std::vector<float> pixelsRgb;
            pixelsRgb.resize(static_cast<size_t>(m_sourceWidth) * static_cast<size_t>(m_sourceHeight) * 3u);
            auto srgbToLinear = [](float x) -> float {
                if (x <= 0.04045f) {
                    return x / 12.92f;
                }
                return std::pow((x + 0.055f) / 1.055f, 2.4f);
            };

            for (size_t i = 0; i < static_cast<size_t>(m_sourceWidth) * static_cast<size_t>(m_sourceHeight); ++i) {
                const size_t src = i * 4u;
                const size_t dst = i * 3u;
                const float r = static_cast<float>(m_sourceLdrRgba8[src + 0]) / 255.0f;
                const float g = static_cast<float>(m_sourceLdrRgba8[src + 1]) / 255.0f;
                const float b = static_cast<float>(m_sourceLdrRgba8[src + 2]) / 255.0f;
                pixelsRgb[dst + 0] = srgbToLinear(r);
                pixelsRgb[dst + 1] = srgbToLinear(g);
                pixelsRgb[dst + 2] = srgbToLinear(b);
            }

            if (!loadFromFloatRgb(std::move(pixelsRgb), m_sourceWidth, m_sourceHeight)) {
                return false;
            }
            DebugLog("Loaded LDR equirect environment map for runtime skybox/IBL generation.\n");
            return true;
        };

        switch (m_skyboxLoadFormat) {
        case SkyboxLoadFormat::HdrEquirect:
            if (!tryLoadHdrFromInput()) {
                DebugLog("Skybox::EnsureHdrEnvironmentLoaded failed: HdrEquirect mode requires HDR input.\n");
                return false;
            }
            return true;
        case SkyboxLoadFormat::LdrEquirect:
            if (!tryLoadLdrFromInput()) {
                DebugLog("Skybox::EnsureHdrEnvironmentLoaded failed: LdrEquirect mode requires LDR input.\n");
                return false;
            }
            return true;
        case SkyboxLoadFormat::Auto:
        default:
            if (tryLoadHdrFromInput()) {
                return true;
            }
            if (tryLoadLdrFromInput()) {
                return true;
            }
            DebugLog("Skybox::EnsureHdrEnvironmentLoaded failed: Auto mode requires HDR or LDR input.\n");
            return false;
        }
    }

    void Skybox::PublishSkyboxSrv(DXGI_FORMAT format)
    {
        if (!m_device || !m_skyboxTexture.IsValid()) {
            return;
        }

        // Derive MipLevels from the uploaded resource itself so both single-mip callers
        // (LDR cubemap / fallback) and the HDR skybox's full mip chain share this path correctly.
        UINT mipLevels = 1;
        if (ID3D12Resource* resource = m_skyboxTexture.Get()) {
            mipLevels = resource->GetDesc().MipLevels;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MipLevels = mipLevels;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        m_device->CreateShaderResourceView(m_skyboxTexture, &srvDesc, m_skyboxSrvCpu);
    }

    bool Skybox::UploadHdrSkyboxTexture(CommandList* cmdList)
    {
        if (!m_device || !cmdList || !m_hdrEquirectLoaded) {
            return false;
        }

        // Timed because the equirect->cube conversion and the mip chain are built on the
        // CPU: the cost scales with the square of the base face size, so this number is
        // what decides whether the result is worth caching on disk.
        ScopedPerfTimer perfTimer("Skybox::UploadHdrSkyboxTexture");

        const UINT skyFaceSize = 2048;

        // Prefer the GPU compute cubemap generator: it produces the same 2048^2 base face
        // with a full mip chain far faster than the CPU box-filter path below. Only try it
        // again after it has failed once, so a failing GPU path doesn't retry (and re-log) every frame;
        // the CPU path remains as a fallback for devices/drivers where the compute path fails.
        if (!m_gpuCubemapGenerationFailed) {
            if (m_cubemapGenerator.IsReady() &&
                m_cubemapGenerator.Generate(cmdList, m_hdrEquirectPixels, m_hdrEquirectWidth, m_hdrEquirectHeight,
                                            skyFaceSize, m_skyboxTexture)) {
                PublishSkyboxSrv(DXGI_FORMAT_R16G16B16A16_FLOAT);
                m_skyboxTextureUploaded = true;
                m_skyboxTextureIsHdr = true;
                return true;
            }
            // Latch the failure so a device/driver that cannot run the compute path
            // does not re-attempt (and re-log) it on every upload; a successful run
            // leaves the latch clear so reloading the environment still uses the GPU.
            m_gpuCubemapGenerationFailed = true;
        }

        // The HDR source equirect is 4K, so a 256^2 base cube face was far too coarse and
        // produced a visibly blurry/aliased sky. Raise the base face to 2048^2 and build a
        // full box-filtered mip chain down to 1x1 so the GPU samples an appropriately
        // filtered level instead of always hitting the sharpest mip. Generation is CPU-side,
        // so this trades startup time and VRAM (RGBA16F, 6 faces * 2048^2 + mips ~= 270MB)
        // for reduced sampling cost and aliasing.
        std::vector<std::vector<float>> skyFaces;
        RendererMathUtility::GenerateSkyCubemapFromEquirect(m_hdrEquirectPixels,
                                                            m_hdrEquirectWidth,
                                                            m_hdrEquirectHeight,
                                                            skyFaceSize,
                                                            skyFaces);

        UINT mipLevels = 1;
        for (UINT size = skyFaceSize; size > 1; size >>= 1) {
            ++mipLevels;
        }

        // Subresource order must match ResourceUploadUtility::CreateTextureCubeFromFloatFacesWithMips
        // (same convention used by IblSystem's prefilter cubemap upload): face-major, mip-minor,
        // index = mip + face * mipLevels.
        std::vector<std::vector<float>> skySubresources(static_cast<size_t>(mipLevels) * 6u);
        for (UINT face = 0; face < 6; ++face) {
            skySubresources[0 + face * mipLevels] = std::move(skyFaces[face]);
            for (UINT mip = 1; mip < mipLevels; ++mip) {
                const UINT prevSize = skyFaceSize >> (mip - 1);
                RendererMathUtility::DownsampleFaceRgbaFloat(skySubresources[(mip - 1) + face * mipLevels],
                                                             prevSize,
                                                             skySubresources[mip + face * mipLevels]);
            }
        }

        if (!ResourceUploadUtility::CreateTextureCubeFromFloatFacesWithMips(*m_device,
                                                     cmdList,
                                                     skySubresources,
                                                     skyFaceSize,
                                                     mipLevels,
                                                     m_skyboxTexture,
                                                     m_skyboxTextureUpload)) {
            return false;
        }

        PublishSkyboxSrv(DXGI_FORMAT_R16G16B16A16_FLOAT);
        m_skyboxTextureUploaded = true;
        m_skyboxTextureIsHdr = true;
        return true;
    }

    bool Skybox::UploadLdrCubemapTexture(CommandList* cmdList)
    {
        if (!m_device || !cmdList ||
            m_sourceType != SourceType::LdrCubemapFaces ||
            m_sourceCubemapFaceRgba8.size() != 6u) {
            return false;
        }

        if (!ResourceUploadUtility::CreateTextureCubeFromRgba8Faces(*m_device,
                                             cmdList,
                                             m_sourceCubemapFaceRgba8,
                                             m_sourceWidth,
                                             m_sourceHeight,
                                             m_skyboxTexture,
                                             m_skyboxTextureUpload)) {
            return false;
        }

        PublishSkyboxSrv(DXGI_FORMAT_R8G8B8A8_UNORM);
        m_skyboxTextureUploaded = true;
        m_skyboxTextureIsHdr = false;
        return true;
    }

    bool Skybox::UploadFallbackSkyboxTexture(CommandList* cmdList)
    {
        if (!m_device || !cmdList) {
            return false;
        }

        std::vector<std::vector<uint8_t>> facePixels;
        static const uint8_t fallbackFaces[6][4] = {
            { 200, 40, 40, 255 },
            { 40, 200, 40, 255 },
            { 40, 40, 200, 255 },
            { 200, 200, 40, 255 },
            { 40, 200, 200, 255 },
            { 200, 40, 200, 255 },
        };

        facePixels.resize(6);
        for (int i = 0; i < 6; ++i) {
            facePixels[i].assign(fallbackFaces[i], fallbackFaces[i] + 4);
        }

        if (!ResourceUploadUtility::CreateTextureCubeFromRgba8Faces(*m_device,
                                             cmdList,
                                             facePixels,
                                             1,
                                             1,
                                             m_skyboxTexture,
                                             m_skyboxTextureUpload)) {
            return false;
        }

        PublishSkyboxSrv(DXGI_FORMAT_R8G8B8A8_UNORM);
        m_skyboxTextureUploaded = true;
        m_skyboxTextureIsHdr = false;
        return true;
    }

    void Skybox::EnsureSkyboxTextureUploaded(CommandList* cmdList)
    {
        if (!m_device || m_skyboxTextureUploaded || m_skyboxUploadAttempted) {
            return;
        }
        m_skyboxUploadAttempted = true;

        if (m_skyboxLoadFormat == SkyboxLoadFormat::CubemapFaces) {
            if (UploadLdrCubemapTexture(cmdList)) {
                return;
            }
            DebugLog("Skybox cubemap face upload failed. Falling back to solid cubemap.\n");
            (void)UploadFallbackSkyboxTexture(cmdList);
            return;
        }

        const bool hasHdrEnvironment = EnsureHdrEnvironmentLoaded();
        if (hasHdrEnvironment && UploadHdrSkyboxTexture(cmdList)) {
            return;
        }
        if (hasHdrEnvironment) {
            DebugLog("Skybox equirect conversion failed. Falling back to solid cubemap.\n");
        }

        (void)UploadFallbackSkyboxTexture(cmdList);
    }

    void Skybox::Render(IRhiCommandEncoder* enc,
                        RenderPipelineStateCache& pipelineStateCache,
                        DescriptorHeap& srvHeap,
                        const Viewport& viewport,
                        const Rect& scissorRect,
                        const float cameraPV[16],
                        const float cameraPos[3],
                        const RenderDirectionalLight& directionalLight,
                        const PushCameraCbCallback& pushCameraCb) const
    {
        if (!enc || !m_skyboxTextureUploaded || !IsSkyboxVBValid()) {
            return;
        }

        enc->SetGraphicsPipelineLayout(RenderPipelineStateCache::MakeLayoutHandle(pipelineStateCache.GetRootSignature()));

        bool useHdrShader = m_skyboxTextureIsHdr;
        if (m_skyboxLoadFormat == SkyboxLoadFormat::LdrEquirect ||
            m_skyboxLoadFormat == SkyboxLoadFormat::CubemapFaces) {
            useHdrShader = false;
        }

        if (useHdrShader) {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetSkyboxHdrPipelineState()));
        } else {
            enc->SetGraphicsPipeline(RenderPipelineStateCache::MakePipelineHandle(pipelineStateCache.GetSkyboxLdrPipelineState()));
        }

        enc->SetViewports(reinterpret_cast<const RhiViewport*>(&viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(&scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        enc->SetDescriptorHeap(RenderPipelineStateCache::MakeDescriptorHeapHandle(srvHeap));
        enc->SetGraphicsDescriptorTable(0, { m_skyboxSrv.ptr });

        float skyboxWorld[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            cameraPos[0], cameraPos[1], cameraPos[2], 1,
        };
        float skyboxMVP[16];
        Mul4x4(skyboxWorld, cameraPV, skyboxMVP);
        if (pushCameraCb) {
            float lightForward[3] = {};
            Math::DirectionFromYawPitch(directionalLight.yaw, directionalLight.pitch, lightForward);

            const float directionalLightDir[4] = {
                -lightForward[0],
                -lightForward[1],
                -lightForward[2],
                0.0f
            };
            const float directionalLightColor[4] = {
                directionalLight.color[0],
                directionalLight.color[1],
                directionalLight.color[2],
                directionalLight.intensity
            };
            const float directionalLightMarkerParams[4] = {
                (m_directionalLightMarkerEnabled && directionalLight.intensity > 0.0f) ? 1.0f : 0.0f,
                m_directionalLightMarkerAngularRadius,
                m_directionalLightMarkerHaloAngularRadius,
                m_directionalLightMarkerBrightness
            };
            const D3D12_GPU_VIRTUAL_ADDRESS cameraCbGpu = pushCameraCb(skyboxMVP,
                                                                       skyboxWorld,
                                                                       directionalLightDir,
                                                                       directionalLightColor,
                                                                       directionalLightMarkerParams);
            if (cameraCbGpu != 0) {
                enc->SetGraphicsConstantBufferView(2, cameraCbGpu);
            }
        }

        if (m_skyboxVbBinding.buffer.IsValid()) {
            enc->SetVertexBufferBindings(0, 1, &m_skyboxVbBinding);
        } else {
            const RhiVertexBufferView rhiVbv{ m_skyboxVBV.BufferLocation, m_skyboxVBV.StrideInBytes, m_skyboxVBV.SizeInBytes };
            enc->SetVertexBuffers(0, 1, &rhiVbv);
        }
        enc->Draw({ static_cast<uint32_t>(_countof(kSkyboxCubeVertices)), 1u, 0u, 0u });
    }
}
