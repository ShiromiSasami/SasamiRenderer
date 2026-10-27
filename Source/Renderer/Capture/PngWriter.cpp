#include "Renderer/Capture/PngWriter.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "Foundation/Tools/DebugOutput.h" // pulls in <windows.h>, needed before <wincodec.h>

#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

namespace SasamiEngine::Render
{
    namespace
    {
        // WIC's PNG encoder is only guaranteed to accept this layout; everything passed in
        // is assumed to already be laid out this way before WritePixels is called, so the
        // codec never has to silently renegotiate a format we didn't ask for.
        const WICPixelFormatGUID kEncodePixelFormat = GUID_WICPixelFormat32bppBGRA;

        std::string FmtHr(HRESULT hr)
        {
            char buf[16] = {};
            std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
            return buf;
        }

        void LogFail(const char* ctx, HRESULT hr)
        {
            std::string msg = ctx;
            msg += " failed. hr=";
            msg += FmtHr(hr);
            msg += "\n";
            DebugLog(msg.c_str());
        }
    }

    bool SavePngBgra8(const std::wstring& path, const uint8_t* bgra, uint32_t width, uint32_t height)
    {
        if (!bgra || width == 0 || height == 0) {
            LogFail("PngWriter::SavePngBgra8 (invalid arguments)", E_INVALIDARG);
            return false;
        }

        // WIC needs COM. The caller may already have initialized it elsewhere. RPC_E_CHANGED_MODE
        // just means some other code owns COM on this thread with a different apartment model --
        // that is not our failure and we must not CoUninitialize what we didn't initialize.
        // S_FALSE (already initialized here with a compatible model) is a SUCCEEDED code and is
        // handled by the same branch as S_OK.
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitializedHere = SUCCEEDED(coHr);
        if (!comInitializedHere && coHr != RPC_E_CHANGED_MODE) {
            LogFail("PngWriter::SavePngBgra8: CoInitializeEx", coHr);
            return false;
        }

        struct ComGuard
        {
            bool owns;
            ~ComGuard()
            {
                if (owns) {
                    CoUninitialize();
                }
            }
        } comGuard{ comInitializedHere };

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: CoCreateInstance(WICImagingFactory)", hr);
            return false;
        }

        const std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(fsPath.parent_path(), ec);
            if (ec) {
                const std::string msg = "PngWriter::SavePngBgra8: cannot create directory \"" +
                                        fsPath.parent_path().string() + "\"\n";
                DebugLog(msg.c_str());
                return false;
            }
        }

        Microsoft::WRL::ComPtr<IWICStream> stream;
        hr = factory->CreateStream(&stream);
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: CreateStream", hr);
            return false;
        }

        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: InitializeFromFilename", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: CreateEncoder", hr);
            return false;
        }

        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: IWICBitmapEncoder::Initialize", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> props;
        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: CreateNewFrame", hr);
            return false;
        }

        hr = frame->Initialize(props.Get());
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: IWICBitmapFrameEncode::Initialize", hr);
            return false;
        }

        hr = frame->SetSize(width, height);
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: SetSize", hr);
            return false;
        }

        WICPixelFormatGUID actualFormat = kEncodePixelFormat;
        hr = frame->SetPixelFormat(&actualFormat);
        if (FAILED(hr) || !IsEqualGUID(actualFormat, kEncodePixelFormat)) {
            LogFail("PngWriter::SavePngBgra8: SetPixelFormat", FAILED(hr) ? hr : E_FAIL);
            return false;
        }

        const UINT stride = width * 4u;
        hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bgra)));
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: WritePixels", hr);
            return false;
        }

        hr = frame->Commit();
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: IWICBitmapFrameEncode::Commit", hr);
            return false;
        }

        hr = encoder->Commit();
        if (FAILED(hr)) {
            LogFail("PngWriter::SavePngBgra8: IWICBitmapEncoder::Commit", hr);
            return false;
        }

        return true;
    }
}
