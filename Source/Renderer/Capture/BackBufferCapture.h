#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

namespace SasamiRenderer
{
    // Holds the WIC imaging factory. Defined in the .cpp so <wincodec.h> and its COM types
    // stay out of this header: Renderer.h includes this file, and a ComPtr member of an
    // incomplete interface type cannot be destroyed by any translation unit that has not
    // also seen the full interface declaration.
    struct WicEncoder;

    // Captures the renderer's back buffer to a PNG on disk.
    //
    // Reads the actual rendered pixels rather than grabbing the window: no window border,
    // no title bar, and unaffected by other windows overlapping or by DWM composition.
    // (Build/ss.ps1's PrintWindow-based capture has all three problems; this does not.)
    class BackBufferCapture
    {
    public:
        // Both defined in the .cpp: a member defaulted in-class is inline in every
        // translation unit, which would instantiate unique_ptr<WicEncoder>'s deleter here
        // where WicEncoder is still incomplete.
        BackBufferCapture();
        ~BackBufferCapture();

        bool Initialize(ID3D12Device* device);
        void Shutdown();

        // Records the copy into cmdList. The caller must have transitioned `backBuffer`
        // to D3D12_RESOURCE_STATE_COPY_SOURCE and must restore its state afterwards.
        // Returns false and records nothing on failure.
        bool RecordCopy(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* backBuffer);

        // Encodes the most recently copied frame. MUST be called only after the GPU has
        // finished executing the command list passed to RecordCopy -- reading earlier
        // returns garbage or a torn frame.
        bool SaveToPng(const std::wstring& path);

        bool HasPendingCapture() const { return m_hasPendingCapture; }

    private:
        // Creates (or recreates, e.g. on window resize) the readback buffer so it can hold
        // at least requiredSize bytes.
        bool EnsureReadbackBuffer(UINT64 requiredSize);

        Microsoft::WRL::ComPtr<ID3D12Device>   m_device;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_readbackBuffer;
        std::unique_ptr<WicEncoder>            m_wic;

        // Layout of the most recently copied frame inside m_readbackBuffer. RowPitch is
        // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-aligned and is almost never equal to width * 4;
        // SaveToPng must walk rows using it rather than assuming a packed layout.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint = {};
        UINT64     m_readbackBufferSize = 0;
        UINT       m_width  = 0;
        UINT       m_height = 0;
        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;

        bool m_initialized        = false;
        bool m_hasPendingCapture  = false;
        bool m_comInitializedByUs = false;
    };
}
