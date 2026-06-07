// Dx12GraphicsDevice_Init.cpp
// Destructor, Initialize, Getters, WaitForGPU.
#include "Renderer/Backends/DirectX12/Dx12GraphicsDevice.h"
#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include <windows.h>
#include <debugapi.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstring>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"


namespace SasamiRenderer
{
#if RHI_DIRECTX12
    namespace
    {
        DXGI_FORMAT ToDxgiFormat(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R8UNorm: return DXGI_FORMAT_R8_UNORM;
            case RhiFormat::R8G8B8A8UNorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case RhiFormat::B8G8R8A8UNorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case RhiFormat::R16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case RhiFormat::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
            case RhiFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
            case RhiFormat::R32UInt: return DXGI_FORMAT_R32_UINT;
            case RhiFormat::D32Float: return DXGI_FORMAT_D32_FLOAT;
            case RhiFormat::D24UNormS8UInt: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        D3D12_RESOURCE_STATES ToDx12State(RhiResourceState state)
        {
            switch (state) {
            case RhiResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RhiResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RhiResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
            case RhiResourceState::ShaderResource:
                return static_cast<D3D12_RESOURCE_STATES>(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            case RhiResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RhiResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case RhiResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
            case RhiResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
            case RhiResourceState::Common:
            default: return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        D3D12_HEAP_TYPE ToDx12HeapType(RhiMemoryUsage usage)
        {
            switch (usage) {
            case RhiMemoryUsage::CpuToGpu: return D3D12_HEAP_TYPE_UPLOAD;
            case RhiMemoryUsage::GpuToCpu: return D3D12_HEAP_TYPE_READBACK;
            case RhiMemoryUsage::GpuOnly:
            default: return D3D12_HEAP_TYPE_DEFAULT;
            }
        }

        D3D12_RESOURCE_FLAGS ToDx12TextureFlags(RhiTextureUsageFlags usage)
        {
            UINT flags = D3D12_RESOURCE_FLAG_NONE;
            if (HasFlag(usage, RhiTextureUsageFlags::RenderTarget)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::DepthStencil)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::UnorderedAccess)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            return static_cast<D3D12_RESOURCE_FLAGS>(flags);
        }

        D3D12_SRV_DIMENSION ToDx12SrvDimension(RhiTextureViewDimension dimension)
        {
            switch (dimension) {
            case RhiTextureViewDimension::Texture1D: return D3D12_SRV_DIMENSION_TEXTURE1D;
            case RhiTextureViewDimension::Texture1DArray: return D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            case RhiTextureViewDimension::Texture2DArray: return D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            case RhiTextureViewDimension::Texture3D: return D3D12_SRV_DIMENSION_TEXTURE3D;
            case RhiTextureViewDimension::TextureCube: return D3D12_SRV_DIMENSION_TEXTURECUBE;
            case RhiTextureViewDimension::TextureCubeArray: return D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            case RhiTextureViewDimension::Texture2D:
            default: return D3D12_SRV_DIMENSION_TEXTURE2D;
            }
        }

        D3D12_DESCRIPTOR_HEAP_TYPE ToDx12DescriptorHeapType(RhiDescriptorHeapType type)
        {
            switch (type) {
            case RhiDescriptorHeapType::Sampler: return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            case RhiDescriptorHeapType::RenderTarget: return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            case RhiDescriptorHeapType::DepthStencil: return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            case RhiDescriptorHeapType::CbvSrvUav:
            default: return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            }
        }

        D3D12_COMMAND_LIST_TYPE ToDx12CommandListType(RhiQueueType queueType)
        {
            switch (queueType) {
            case RhiQueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
            case RhiQueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
            case RhiQueueType::Graphics:
            case RhiQueueType::Present:
            default: return D3D12_COMMAND_LIST_TYPE_DIRECT;
            }
        }

        D3D12_SHADER_VISIBILITY ToDx12ShaderVisibility(RhiShaderStageFlags visibility)
        {
            const uint32_t flags = static_cast<uint32_t>(visibility);
            if (flags == static_cast<uint32_t>(RhiShaderStageFlags::Vertex)) {
                return D3D12_SHADER_VISIBILITY_VERTEX;
            }
            if (flags == static_cast<uint32_t>(RhiShaderStageFlags::Pixel)) {
                return D3D12_SHADER_VISIBILITY_PIXEL;
            }
            if (flags == static_cast<uint32_t>(RhiShaderStageFlags::Compute)) {
                return D3D12_SHADER_VISIBILITY_ALL;
            }
            return D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_DESCRIPTOR_RANGE_TYPE ToDx12DescriptorRangeType(RhiBindingType type)
        {
            switch (type) {
            case RhiBindingType::ConstantBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            case RhiBindingType::UnorderedAccess: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            case RhiBindingType::Sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            case RhiBindingType::ShaderResource:
            default: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            }
        }

        D3D12_ROOT_PARAMETER_TYPE ToDx12RootDescriptorType(RhiBindingType type)
        {
            switch (type) {
            case RhiBindingType::ConstantBuffer: return D3D12_ROOT_PARAMETER_TYPE_CBV;
            case RhiBindingType::UnorderedAccess: return D3D12_ROOT_PARAMETER_TYPE_UAV;
            case RhiBindingType::ShaderResource: return D3D12_ROOT_PARAMETER_TYPE_SRV;
            case RhiBindingType::RootConstants: return D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            case RhiBindingType::Sampler:
            default: return D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            }
        }

        D3D12_COMPARISON_FUNC ToDx12CompareOp(RhiCompareOp op)
        {
            switch (op) {
            case RhiCompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
            case RhiCompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
            case RhiCompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
            case RhiCompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case RhiCompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
            case RhiCompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case RhiCompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case RhiCompareOp::Always:
            default: return D3D12_COMPARISON_FUNC_ALWAYS;
            }
        }

        D3D12_PRIMITIVE_TOPOLOGY_TYPE ToDx12PrimitiveTopologyType(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::LineList:
            case RhiPrimitiveTopology::LineStrip:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            case RhiPrimitiveTopology::PointList:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            case RhiPrimitiveTopology::PatchList:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            case RhiPrimitiveTopology::TriangleList:
            case RhiPrimitiveTopology::TriangleStrip:
            default:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }
        }

        D3D_PRIMITIVE_TOPOLOGY ToDx12PrimitiveTopology(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            case RhiPrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case RhiPrimitiveTopology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case RhiPrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RhiPrimitiveTopology::PatchList: return D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            case RhiPrimitiveTopology::TriangleList:
            default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            }
        }

        D3D12_CULL_MODE ToDx12CullMode(RhiCullMode mode)
        {
            switch (mode) {
            case RhiCullMode::None: return D3D12_CULL_MODE_NONE;
            case RhiCullMode::Front: return D3D12_CULL_MODE_FRONT;
            case RhiCullMode::Back:
            default: return D3D12_CULL_MODE_BACK;
            }
        }

        D3D12_FILL_MODE ToDx12FillMode(RhiFillMode mode)
        {
            return mode == RhiFillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        }

    }

    Dx12GraphicsDevice::~Dx12GraphicsDevice()
    {
        if (m_fenceEvent) {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
    }

    bool Dx12GraphicsDevice::Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount)
    {
        m_bufferCount = bufferCount;

        auto cleanup = [&]() {
            if (m_fenceEvent) {
                CloseHandle(m_fenceEvent);
                m_fenceEvent = nullptr;
            }
            m_fence.Reset();
            m_swapChain.m_swapChain.Reset();
            m_computeQueue.m_queue.Reset();
            m_commandQueue.m_queue.Reset();
            m_rayTracingDevice.Reset();
            m_device.Reset();
            m_supportsHardwareRayTracing = false;
            m_capabilities = {};
        };


#if defined(_DEBUG)
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
                DebugLog("Dx12GraphicsDevice::Initialize: D3D12 debug layer enabled.\n");
            }
        }

#endif
        ComPtr<IDXGIFactory6> factory;
        HRESULT result = CreateDXGIFactory(IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateDXGIFactory failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selectedLevel = static_cast<D3D_FEATURE_LEVEL>(0);
        for (D3D_FEATURE_LEVEL level : featureLevels) {
            result = D3D12CreateDevice(nullptr, level, IID_PPV_ARGS(&m_device));
            if (SUCCEEDED(result)) {
                selectedLevel = level;
                break;
            }
        }
        if (!m_device) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: D3D12CreateDevice failed for FL 12_1/12_0/11_1/11_0.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }
        {
            char msg[128] = {};
            std::snprintf(msg, sizeof(msg),
                          "Dx12GraphicsDevice::Initialize: D3D12 device created (feature level: 0x%04X).\n",
                          static_cast<unsigned int>(selectedLevel));
            DebugLog(msg);
        }

        m_supportsHardwareRayTracing = false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                    &options5,
                                                    sizeof(options5))) &&
            options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 &&
            SUCCEEDED(m_device.As(&m_rayTracingDevice))) {
            m_supportsHardwareRayTracing = true;
            DebugLog("Dx12GraphicsDevice::Initialize: hardware ray tracing is supported.\n");
        } else {
            m_rayTracingDevice.Reset();
            DebugLog("Dx12GraphicsDevice::Initialize: hardware ray tracing is not supported.\n");
        }

        D3D12_COMMAND_QUEUE_DESC cqDesc = {};
        cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result = m_device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&m_commandQueue.m_queue));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateCommandQueue failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC cqComputeDesc = {};
        cqComputeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        result = m_device->CreateCommandQueue(&cqComputeDesc, IID_PPV_ARGS(&m_computeQueue.m_queue));
        if (FAILED(result)) {
            DebugLog("Dx12GraphicsDevice::Initialize: CreateCommandQueue (COMPUTE) failed. Async compute will be unavailable.\n");
            m_computeQueue.m_queue.Reset(); // non-fatal: fall back to no compute queue
        }

        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.BufferCount = m_bufferCount;
        scDesc.Width = width;
        scDesc.Height = height;
        scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain1;
        factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

        result = factory->CreateSwapChainForHwnd(m_commandQueue.m_queue.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateSwapChainForHwnd failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            swapChain1.Reset();
            cleanup();
            return false;
        }
        swapChain1.As(&m_swapChain.m_swapChain);

        result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        if (FAILED(result)) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateFence failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }
        m_fenceValue = 0;
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent) {
            DebugLogDialog("Dx12GraphicsDevice::Initialize: CreateEvent failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            cleanup();
            return false;
        }

        m_capabilities = {};
        m_capabilities.api = RhiBackendApi::DirectX12;
        m_capabilities.supportsGraphicsQueue = true;
        m_capabilities.supportsComputeQueue = (m_computeQueue.m_queue != nullptr);
        m_capabilities.supportsSwapChain = true;
        m_capabilities.supportsNativeFrame = false;
        m_capabilities.supportsFeatureRenderPasses = true;
        m_capabilities.supportsD3D12CompatibilitySurface = true;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;
        m_capabilities.supportsHardwareRayTracing = m_supportsHardwareRayTracing;

        return true;
    }

    GraphicsRuntime Dx12GraphicsDevice::GetBackend() const
    {
        return GraphicsRuntime::DirectX12;
    }

    void* Dx12GraphicsDevice::GetNativeDeviceHandle() const
    {
        return m_device.Get();
    }

    void* Dx12GraphicsDevice::GetNativeGraphicsQueueHandle() const
    {
        return m_commandQueue.m_queue.Get();
    }

    ID3D12Device* Dx12GraphicsDevice::GetDevice() const
    {
        return m_device.Get();
    }

    ID3D12Device5* Dx12GraphicsDevice::GetRayTracingDevice() const
    {
        return m_rayTracingDevice.Get();
    }

    const RhiBackendCapabilities& Dx12GraphicsDevice::GetCapabilities() const
    {
        return m_capabilities;
    }

    bool Dx12GraphicsDevice::SupportsHardwareRayTracing() const
    {
        return m_supportsHardwareRayTracing;
    }

    CommandQueue& Dx12GraphicsDevice::GetCommandQueue()
    {
        return m_commandQueue;
    }

    CommandQueue& Dx12GraphicsDevice::GetComputeQueue()
    {
        return m_computeQueue;
    }

    SwapChain& Dx12GraphicsDevice::GetSwapChain()
    {
        return m_swapChain;
    }

    UINT Dx12GraphicsDevice::GetDescriptorHandleIncrementSize(DescriptorHeapType type) const
    {
        return m_device->GetDescriptorHandleIncrementSize(type);
    }

    void Dx12GraphicsDevice::WaitForGPU()
    {
        if (!m_commandQueue.m_queue || !m_fence) return;
        const UINT64 v = ++m_fenceValue;
        if (FAILED(m_commandQueue.m_queue->Signal(m_fence.Get(), v))) return;
        if (m_fence->GetCompletedValue() < v) {
            if (FAILED(m_fence->SetEventOnCompletion(v, m_fenceEvent))) return;
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

#endif
} // namespace SasamiRenderer
