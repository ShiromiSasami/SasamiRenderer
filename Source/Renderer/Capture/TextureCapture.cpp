#include "Renderer/Capture/TextureCapture.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

#include "Renderer/Capture/PngWriter.h"
#include "Renderer/Resources/RenderPipelineStateCacheLog.h" // pulls in <windows.h>, needed before <wincodec.h>

#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace SasamiEngine::Render
{
    // Out-of-line so the header can forward-declare it (see TextureCapture.h).
    struct WicEncoder
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    };

    namespace
    {
        // Which family a captured format belongs to, so SaveToPng knows how to turn its raw
        // bytes into 8-bit BGRA for the PNG encoder.
        enum class CaptureKind
        {
            Rgba8,
            Bgra8,
            Depth32,
            Rgba16Float,
        };

        // Recognizes the formats a captured texture can realistically be. `swapRedBlue` is
        // only meaningful for the 8-bit-per-channel kinds and reports whether R/B need to be
        // exchanged to reach the BGRA byte order WIC expects. Anything else (10-bit, ...) is
        // reported as unsupported rather than silently producing a wrong-colored image.
        bool ClassifyCaptureFormat(DXGI_FORMAT format, CaptureKind& kind, bool& swapRedBlue)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                kind = CaptureKind::Rgba8;
                swapRedBlue = true;
                return true;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                kind = CaptureKind::Bgra8;
                swapRedBlue = false;
                return true;
            case DXGI_FORMAT_R32_TYPELESS:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT:
                kind = CaptureKind::Depth32;
                swapRedBlue = false;
                return true;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                kind = CaptureKind::Rgba16Float;
                swapRedBlue = false;
                return true;
            default:
                return false;
            }
        }

        // IEEE 754 binary16 -> binary32. Handles subnormals and Inf/NaN so no half bit
        // pattern produces garbage; there is no external half-float dependency to reach for.
        float HalfToFloat(uint16_t h)
        {
            uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
            uint32_t exponent = (h >> 10) & 0x1Fu;
            uint32_t mantissa = h & 0x03FFu;
            uint32_t bits;

            if (exponent == 0) {
                if (mantissa == 0) {
                    bits = sign; // +/-0
                } else {
                    // Subnormal half: normalize into a normal float.
                    int32_t e = 1;
                    while ((mantissa & 0x0400u) == 0) {
                        mantissa <<= 1;
                        --e;
                    }
                    mantissa &= 0x03FFu;
                    bits = sign | (static_cast<uint32_t>(e + (127 - 15)) << 23) | (mantissa << 13);
                }
            } else if (exponent == 0x1Fu) {
                // Inf or NaN: keep the exponent saturated and preserve NaN payload bits.
                bits = sign | 0x7F800000u | (mantissa << 13);
            } else {
                bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
            }

            return std::bit_cast<float>(bits);
        }

        std::string FormatToDebugString(DXGI_FORMAT format)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
            case DXGI_FORMAT_B8G8R8A8_UNORM:      return "B8G8R8A8_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
            case DXGI_FORMAT_R10G10B10A2_UNORM:   return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_R16G16B16A16_FLOAT:  return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R32_TYPELESS:        return "R32_TYPELESS";
            case DXGI_FORMAT_R32_FLOAT:           return "R32_FLOAT";
            case DXGI_FORMAT_D32_FLOAT:           return "D32_FLOAT";
            case DXGI_FORMAT_UNKNOWN:             return "UNKNOWN";
            default:
                return "DXGI_FORMAT(" + std::to_string(static_cast<int>(format)) + ")";
            }
        }
    }

    TextureCapture::TextureCapture() = default;

    TextureCapture::~TextureCapture()
    {
        Shutdown();
    }

    bool TextureCapture::Initialize(ID3D12Device* device)
    {
        Shutdown();

        if (!device) {
            LogFail("TextureCapture::Initialize (null device)", E_INVALIDARG);
            return false;
        }

        // WIC needs COM. The app may already have initialized it elsewhere (Renderer::
        // Initialize does, with COINIT_MULTITHREADED). RPC_E_CHANGED_MODE just means some
        // other code owns COM on this thread with a different apartment model -- that is
        // not our failure and we must not CoUninitialize what we didn't initialize. S_FALSE
        // (already initialized here with a compatible model) is a SUCCEEDED code and is
        // handled by the same branch as S_OK.
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(coHr)) {
            m_comInitializedByUs = true;
        } else if (coHr != RPC_E_CHANGED_MODE) {
            LogFail("TextureCapture::Initialize: CoInitializeEx", coHr);
            return false;
        }

        m_wic = std::make_unique<WicEncoder>();

        const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                            nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&m_wic->factory));
        if (FAILED(hr)) {
            LogFail("TextureCapture::Initialize: CoCreateInstance(WICImagingFactory)", hr);
            m_wic.reset();
            if (m_comInitializedByUs) {
                CoUninitialize();
                m_comInitializedByUs = false;
            }
            return false;
        }

        m_device = device;
        m_initialized = true;
        return true;
    }

    void TextureCapture::Shutdown()
    {
        m_readbackBuffer.Reset();
        m_wic.reset();
        m_device.Reset();

        m_footprint = {};
        m_readbackBufferSize = 0;
        m_width = 0;
        m_height = 0;
        m_format = DXGI_FORMAT_UNKNOWN;

        m_initialized = false;
        m_hasPendingCapture = false;

        if (m_comInitializedByUs) {
            CoUninitialize();
            m_comInitializedByUs = false;
        }
    }

    bool TextureCapture::EnsureReadbackBuffer(UINT64 requiredSize)
    {
        if (m_readbackBuffer && m_readbackBufferSize >= requiredSize) {
            return true;
        }

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width            = requiredSize;
        bufferDesc.Height           = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels        = 1;
        bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        m_readbackBuffer.Reset();
        m_readbackBufferSize = 0;

        const HRESULT hr = m_device->CreateCommittedResource(&heapProps,
                                                              D3D12_HEAP_FLAG_NONE,
                                                              &bufferDesc,
                                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                                              nullptr,
                                                              IID_PPV_ARGS(&m_readbackBuffer));
        if (FAILED(hr)) {
            LogFail("TextureCapture::EnsureReadbackBuffer: CreateCommittedResource", hr);
            return false;
        }

        m_readbackBufferSize = requiredSize;
        return true;
    }

    bool TextureCapture::RecordCopy(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* texture, UINT subresource)
    {
        // A failed record must never leave a stale "pending" flag pointing at a readback
        // buffer that was never actually written this time around.
        m_hasPendingCapture = false;

        if (!m_initialized || !cmdList || !texture) {
            return false;
        }

        const D3D12_RESOURCE_DESC desc = texture->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT   numRows      = 0;
        UINT64 rowSizeBytes = 0;
        UINT64 totalBytes   = 0;
        m_device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);
        if (totalBytes == 0) {
            LogFail("TextureCapture::RecordCopy: GetCopyableFootprints (zero size)", E_INVALIDARG);
            return false;
        }

        // Resize (window resize, or first use) whenever the readback buffer is too small.
        // GetCopyableFootprints already rounded RowPitch up to
        // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, so totalBytes reflects that padding.
        if (!EnsureReadbackBuffer(totalBytes)) {
            return false;
        }

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_readbackBuffer.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = texture;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = subresource;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        m_footprint = footprint;
        m_width     = static_cast<UINT>(desc.Width);
        m_height    = desc.Height;
        m_format    = desc.Format;
        m_hasPendingCapture = true;
        return true;
    }

    bool TextureCapture::SaveToPng(const std::wstring& path)
    {
        if (!m_initialized || !m_hasPendingCapture || !m_readbackBuffer) {
            LogFail("TextureCapture::SaveToPng (no pending capture)", E_FAIL);
            return false;
        }

        CaptureKind kind = CaptureKind::Rgba8;
        bool swapRedBlue = false;
        if (!ClassifyCaptureFormat(m_format, kind, swapRedBlue)) {
            const std::string msg = "TextureCapture::SaveToPng: unsupported texture format " +
                                    FormatToDebugString(m_format) + "\n";
            DebugLog(msg.c_str());
            m_hasPendingCapture = false;
            return false;
        }

        const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(m_readbackBufferSize) };
        void* mapped = nullptr;
        HRESULT hr = m_readbackBuffer->Map(0, &readRange, &mapped);
        if (FAILED(hr) || !mapped) {
            LogFail("TextureCapture::SaveToPng: Map", hr);
            m_hasPendingCapture = false;
            return false;
        }

        // Deswizzle row-by-row using the actual (aligned) RowPitch: the readback buffer's
        // rows are padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, so treating it as a tightly
        // packed width*4 buffer would read each row from the wrong offset and skew the image.
        std::vector<uint8_t> pixels(static_cast<size_t>(m_width) * m_height * 4u);
        const uint8_t* base    = static_cast<const uint8_t*>(mapped) + m_footprint.Offset;
        const UINT     rowPitch = m_footprint.Footprint.RowPitch;

        switch (kind) {
        case CaptureKind::Rgba8:
        case CaptureKind::Bgra8:
            for (UINT y = 0; y < m_height; ++y) {
                const uint8_t* srcRow = base + static_cast<size_t>(y) * rowPitch;
                uint8_t*       dstRow = pixels.data() + static_cast<size_t>(y) * m_width * 4u;

                if (swapRedBlue) {
                    for (UINT x = 0; x < m_width; ++x) {
                        const uint8_t* s = srcRow + static_cast<size_t>(x) * 4u;
                        uint8_t*       d = dstRow + static_cast<size_t>(x) * 4u;
                        d[0] = s[2]; // B
                        d[1] = s[1]; // G
                        d[2] = s[0]; // R
                        d[3] = s[3]; // A
                    }
                } else {
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(m_width) * 4u);
                }
            }
            break;

        case CaptureKind::Depth32:
            // This renderer uses reverse-Z (near = 1.0, far = 0.0), so nearer surfaces come
            // out whiter. No auto-normalization (min/max scaling): the same depth value must
            // always map to the same pixel value so dumps can be compared numerically.
            for (UINT y = 0; y < m_height; ++y) {
                const uint8_t* srcRow = base + static_cast<size_t>(y) * rowPitch;
                uint8_t*       dstRow = pixels.data() + static_cast<size_t>(y) * m_width * 4u;

                for (UINT x = 0; x < m_width; ++x) {
                    float value;
                    std::memcpy(&value, srcRow + static_cast<size_t>(x) * 4u, sizeof(value));

                    const float   d = std::clamp(value, 0.0f, 1.0f);
                    const uint8_t g = static_cast<uint8_t>(d * 255.0f + 0.5f);

                    uint8_t* dst = dstRow + static_cast<size_t>(x) * 4u;
                    dst[0] = g; // B
                    dst[1] = g; // G
                    dst[2] = g; // R
                    dst[3] = 255; // A
                }
            }
            break;

        case CaptureKind::Rgba16Float:
            // Values above 1.0 are simply clamped -- no tone mapping is applied.
            for (UINT y = 0; y < m_height; ++y) {
                const uint8_t* srcRow = base + static_cast<size_t>(y) * rowPitch;
                uint8_t*       dstRow = pixels.data() + static_cast<size_t>(y) * m_width * 4u;

                for (UINT x = 0; x < m_width; ++x) {
                    uint16_t half[4];
                    std::memcpy(half, srcRow + static_cast<size_t>(x) * 8u, sizeof(half));

                    const float r = std::clamp(HalfToFloat(half[0]), 0.0f, 1.0f);
                    const float g = std::clamp(HalfToFloat(half[1]), 0.0f, 1.0f);
                    const float b = std::clamp(HalfToFloat(half[2]), 0.0f, 1.0f);
                    const float a = std::clamp(HalfToFloat(half[3]), 0.0f, 1.0f);

                    uint8_t* dst = dstRow + static_cast<size_t>(x) * 4u;
                    dst[0] = static_cast<uint8_t>(b * 255.0f + 0.5f); // B
                    dst[1] = static_cast<uint8_t>(g * 255.0f + 0.5f); // G
                    dst[2] = static_cast<uint8_t>(r * 255.0f + 0.5f); // R
                    dst[3] = static_cast<uint8_t>(a * 255.0f + 0.5f); // A
                }
            }
            break;
        }

        const D3D12_RANGE emptyWrittenRange = { 0, 0 };
        m_readbackBuffer->Unmap(0, &emptyWrittenRange);
        m_hasPendingCapture = false;

        return SavePngBgra8(path, pixels.data(), m_width, m_height);
    }
}
