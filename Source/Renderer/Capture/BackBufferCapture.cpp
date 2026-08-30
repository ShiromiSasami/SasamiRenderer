#include "Renderer/Capture/BackBufferCapture.h"

#include <cstring>
#include <filesystem>
#include <vector>

#include "Renderer/Resources/RenderPipelineStateCacheLog.h" // pulls in <windows.h>, needed before <wincodec.h>

#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

namespace SasamiRenderer
{
    // Out-of-line so the header can forward-declare it (see BackBufferCapture.h).
    struct WicEncoder
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    };

    namespace
    {
        // WIC's PNG encoder is only guaranteed to accept this layout; everything read back
        // from the GPU is converted into it before WritePixels so the codec never has to
        // silently renegotiate a format we didn't ask for.
        const WICPixelFormatGUID kEncodePixelFormat = GUID_WICPixelFormat32bppBGRA;

        // Recognizes the handful of 8-bit-per-channel formats a swap-chain back buffer can
        // realistically be. Sets swapRedBlue to whether R/B need to be exchanged to reach
        // the BGRA byte order WIC expects. Anything else (10-bit, float HDR formats, ...) is
        // reported as unsupported rather than silently producing a wrong-colored image.
        bool ClassifyBackBufferFormat(DXGI_FORMAT format, bool& swapRedBlue)
        {
            switch (format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                swapRedBlue = true;
                return true;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                swapRedBlue = false;
                return true;
            default:
                return false;
            }
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
            case DXGI_FORMAT_UNKNOWN:             return "UNKNOWN";
            default:
                return "DXGI_FORMAT(" + std::to_string(static_cast<int>(format)) + ")";
            }
        }
    }

    BackBufferCapture::BackBufferCapture() = default;

    BackBufferCapture::~BackBufferCapture()
    {
        Shutdown();
    }

    bool BackBufferCapture::Initialize(ID3D12Device* device)
    {
        Shutdown();

        if (!device) {
            LogFail("BackBufferCapture::Initialize (null device)", E_INVALIDARG);
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
            LogFail("BackBufferCapture::Initialize: CoInitializeEx", coHr);
            return false;
        }

        m_wic = std::make_unique<WicEncoder>();

        const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                            nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&m_wic->factory));
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::Initialize: CoCreateInstance(WICImagingFactory)", hr);
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

    void BackBufferCapture::Shutdown()
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

    bool BackBufferCapture::EnsureReadbackBuffer(UINT64 requiredSize)
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
            LogFail("BackBufferCapture::EnsureReadbackBuffer: CreateCommittedResource", hr);
            return false;
        }

        m_readbackBufferSize = requiredSize;
        return true;
    }

    bool BackBufferCapture::RecordCopy(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* backBuffer)
    {
        // A failed record must never leave a stale "pending" flag pointing at a readback
        // buffer that was never actually written this time around.
        m_hasPendingCapture = false;

        if (!m_initialized || !cmdList || !backBuffer) {
            return false;
        }

        const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT   numRows      = 0;
        UINT64 rowSizeBytes = 0;
        UINT64 totalBytes   = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);
        if (totalBytes == 0) {
            LogFail("BackBufferCapture::RecordCopy: GetCopyableFootprints (zero size)", E_INVALIDARG);
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
        src.pResource        = backBuffer;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        m_footprint = footprint;
        m_width     = static_cast<UINT>(desc.Width);
        m_height    = desc.Height;
        m_format    = desc.Format;
        m_hasPendingCapture = true;
        return true;
    }

    bool BackBufferCapture::SaveToPng(const std::wstring& path)
    {
        if (!m_initialized || !m_hasPendingCapture || !m_readbackBuffer) {
            LogFail("BackBufferCapture::SaveToPng (no pending capture)", E_FAIL);
            return false;
        }

        bool swapRedBlue = false;
        if (!ClassifyBackBufferFormat(m_format, swapRedBlue)) {
            const std::string msg = "BackBufferCapture::SaveToPng: unsupported back buffer format " +
                                    FormatToDebugString(m_format) + "\n";
            DebugLog(msg.c_str());
            m_hasPendingCapture = false;
            return false;
        }

        const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(m_readbackBufferSize) };
        void* mapped = nullptr;
        HRESULT hr = m_readbackBuffer->Map(0, &readRange, &mapped);
        if (FAILED(hr) || !mapped) {
            LogFail("BackBufferCapture::SaveToPng: Map", hr);
            m_hasPendingCapture = false;
            return false;
        }

        // Deswizzle row-by-row using the actual (aligned) RowPitch: the readback buffer's
        // rows are padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, so treating it as a tightly
        // packed width*4 buffer would read each row from the wrong offset and skew the image.
        std::vector<uint8_t> pixels(static_cast<size_t>(m_width) * m_height * 4u);
        const uint8_t* base    = static_cast<const uint8_t*>(mapped) + m_footprint.Offset;
        const UINT     rowPitch = m_footprint.Footprint.RowPitch;

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

        const D3D12_RANGE emptyWrittenRange = { 0, 0 };
        m_readbackBuffer->Unmap(0, &emptyWrittenRange);
        m_hasPendingCapture = false;

        const std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(fsPath.parent_path(), ec);
            if (ec) {
                const std::string msg = "BackBufferCapture::SaveToPng: cannot create directory \"" +
                                        fsPath.parent_path().string() + "\"\n";
                DebugLog(msg.c_str());
                return false;
            }
        }

        Microsoft::WRL::ComPtr<IWICStream> stream;
        hr = m_wic->factory->CreateStream(&stream);
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: CreateStream", hr);
            return false;
        }

        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: InitializeFromFilename", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        hr = m_wic->factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: CreateEncoder", hr);
            return false;
        }

        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: IWICBitmapEncoder::Initialize", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> props;
        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: CreateNewFrame", hr);
            return false;
        }

        hr = frame->Initialize(props.Get());
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: IWICBitmapFrameEncode::Initialize", hr);
            return false;
        }

        hr = frame->SetSize(m_width, m_height);
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: SetSize", hr);
            return false;
        }

        WICPixelFormatGUID actualFormat = kEncodePixelFormat;
        hr = frame->SetPixelFormat(&actualFormat);
        if (FAILED(hr) || !IsEqualGUID(actualFormat, kEncodePixelFormat)) {
            LogFail("BackBufferCapture::SaveToPng: SetPixelFormat", FAILED(hr) ? hr : E_FAIL);
            return false;
        }

        const UINT stride = m_width * 4u;
        hr = frame->WritePixels(m_height, stride, static_cast<UINT>(pixels.size()), reinterpret_cast<BYTE*>(pixels.data()));
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: WritePixels", hr);
            return false;
        }

        hr = frame->Commit();
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: IWICBitmapFrameEncode::Commit", hr);
            return false;
        }

        hr = encoder->Commit();
        if (FAILED(hr)) {
            LogFail("BackBufferCapture::SaveToPng: IWICBitmapEncoder::Commit", hr);
            return false;
        }

        return true;
    }
}
