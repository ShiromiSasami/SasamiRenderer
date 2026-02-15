#include "AppFramework/Loader/AssetLoader.h"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cmath>

#include <wincodec.h>
#include <wrl.h>

namespace SasamiRenderer
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
    }

    bool AssetLoader::LoadRgba8ViaWIC(const std::wstring& path,
                                      std::vector<uint8_t>& pixels,
                                      UINT& width,
                                      UINT& height)
    {
        pixels.clear();
        width = 0;
        height = 0;

        ComPtr<IWICImagingFactory> wicFactory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr)) return false;

        hr = wicFactory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr)) return false;

        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) return false;

        hr = wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return false;

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return false;

        UINT w = 0;
        UINT h = 0;
        hr = frame->GetSize(&w, &h);
        if (FAILED(hr) || w == 0 || h == 0) return false;

        pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(hr)) {
            pixels.clear();
            return false;
        }

        width = w;
        height = h;
        return true;
    }

    bool AssetLoader::LoadCubemapFacesViaWIC(const std::array<std::wstring, 6>& paths,
                                             std::vector<std::vector<uint8_t>>& facePixels,
                                             UINT& width,
                                             UINT& height)
    {
        facePixels.clear();
        width = 0;
        height = 0;

        for (size_t i = 0; i < paths.size(); ++i) {
            std::vector<uint8_t> pixels;
            UINT w = 0;
            UINT h = 0;
            if (!LoadRgba8ViaWIC(paths[i], pixels, w, h)) {
                return false;
            }
            if (i == 0) {
                width = w;
                height = h;
            } else if (w != width || h != height) {
                return false;
            }
            facePixels.push_back(std::move(pixels));
        }

        return true;
    }

    bool AssetLoader::LoadRadianceHdr(const std::wstring& path,
                                      std::vector<float>& outRgb,
                                      UINT& outWidth,
                                      UINT& outHeight)
    {
        outRgb.clear();
        outWidth = 0;
        outHeight = 0;

        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        std::string line;
        if (!std::getline(file, line)) {
            return false;
        }
        if (line.find("#?RADIANCE") != 0 && line.find("#?RGBE") != 0) {
            return false;
        }

        bool formatOk = false;
        while (std::getline(file, line)) {
            if (line.empty() || line == "\r") {
                break;
            }
            if (line.find("FORMAT=32-bit_rle_rgbe") != std::string::npos) {
                formatOk = true;
            }
        }
        if (!formatOk) {
            return false;
        }

        do {
            if (!std::getline(file, line)) {
                return false;
            }
        } while (line.empty() || line == "\r");

        int width = 0;
        int height = 0;
        bool minusY = false;
        if (::sscanf_s(line.c_str(), " -Y %d +X %d", &height, &width) == 2) {
            minusY = true;
        } else if (::sscanf_s(line.c_str(), " +Y %d +X %d", &height, &width) == 2) {
            minusY = false;
        } else {
            return false;
        }
        if (width <= 0 || height <= 0) {
            return false;
        }

        outWidth = static_cast<UINT>(width);
        outHeight = static_cast<UINT>(height);
        outRgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u, 0.0f);

        std::vector<uint8_t> scanline(static_cast<size_t>(width) * 4u);
        std::vector<uint8_t> channel(static_cast<size_t>(width));

        for (int y = 0; y < height; ++y) {
            uint8_t rgbe[4] = {};
            file.read(reinterpret_cast<char*>(rgbe), 4);
            if (!file.good()) {
                return false;
            }

            if (width >= 8 && width < 32768 && rgbe[0] == 2 && rgbe[1] == 2 && (rgbe[2] & 0x80) == 0) {
                const int scanWidth = (static_cast<int>(rgbe[2]) << 8) | static_cast<int>(rgbe[3]);
                if (scanWidth != width) {
                    return false;
                }

                for (int c = 0; c < 4; ++c) {
                    int x = 0;
                    while (x < width) {
                        uint8_t count = 0;
                        file.read(reinterpret_cast<char*>(&count), 1);
                        if (!file.good()) {
                            return false;
                        }
                        if (count > 128) {
                            const int runLength = static_cast<int>(count) - 128;
                            uint8_t value = 0;
                            file.read(reinterpret_cast<char*>(&value), 1);
                            if (!file.good() || x + runLength > width) {
                                return false;
                            }
                            for (int i = 0; i < runLength; ++i) {
                                channel[static_cast<size_t>(x++)] = value;
                            }
                        } else {
                            const int runLength = static_cast<int>(count);
                            if (runLength <= 0 || x + runLength > width) {
                                return false;
                            }
                            file.read(reinterpret_cast<char*>(&channel[static_cast<size_t>(x)]), runLength);
                            if (!file.good()) {
                                return false;
                            }
                            x += runLength;
                        }
                    }
                    for (int x = 0; x < width; ++x) {
                        scanline[static_cast<size_t>(x) * 4u + static_cast<size_t>(c)] = channel[static_cast<size_t>(x)];
                    }
                }
            } else {
                scanline[0] = rgbe[0];
                scanline[1] = rgbe[1];
                scanline[2] = rgbe[2];
                scanline[3] = rgbe[3];
                file.read(reinterpret_cast<char*>(&scanline[4]), static_cast<std::streamsize>((width - 1) * 4));
                if (!file.good()) {
                    return false;
                }
            }

            const int dstY = minusY ? y : (height - 1 - y);
            for (int x = 0; x < width; ++x) {
                const uint8_t r = scanline[static_cast<size_t>(x) * 4u + 0u];
                const uint8_t g = scanline[static_cast<size_t>(x) * 4u + 1u];
                const uint8_t b = scanline[static_cast<size_t>(x) * 4u + 2u];
                const uint8_t e = scanline[static_cast<size_t>(x) * 4u + 3u];
                const size_t dst = (static_cast<size_t>(dstY) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u;
                if (e == 0) {
                    outRgb[dst + 0] = 0.0f;
                    outRgb[dst + 1] = 0.0f;
                    outRgb[dst + 2] = 0.0f;
                } else {
                    const float scale = std::ldexp(1.0f, static_cast<int>(e) - (128 + 8));
                    outRgb[dst + 0] = static_cast<float>(r) * scale;
                    outRgb[dst + 1] = static_cast<float>(g) * scale;
                    outRgb[dst + 2] = static_cast<float>(b) * scale;
                }
            }
        }

        return true;
    }
}
