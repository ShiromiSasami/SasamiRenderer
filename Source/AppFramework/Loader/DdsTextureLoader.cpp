#include "Loader/DdsTextureLoader.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "Foundation/Tools/DebugOutput.h"

#define BCDEC_IMPLEMENTATION
#include "bcdec/bcdec.h"

namespace SasamiRenderer
{
    namespace
    {
        constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
        {
            return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
        }

        constexpr uint32_t kDdsMagic = MakeFourCC('D', 'D', 'S', ' ');

        constexpr uint32_t kFourCcDXT1 = MakeFourCC('D', 'X', 'T', '1');
        constexpr uint32_t kFourCcDXT2 = MakeFourCC('D', 'X', 'T', '2');
        constexpr uint32_t kFourCcDXT3 = MakeFourCC('D', 'X', 'T', '3');
        constexpr uint32_t kFourCcDXT4 = MakeFourCC('D', 'X', 'T', '4');
        constexpr uint32_t kFourCcDXT5 = MakeFourCC('D', 'X', 'T', '5');
        constexpr uint32_t kFourCcATI1 = MakeFourCC('A', 'T', 'I', '1');
        constexpr uint32_t kFourCcATI2 = MakeFourCC('A', 'T', 'I', '2');
        constexpr uint32_t kFourCcBC4U = MakeFourCC('B', 'C', '4', 'U');
        constexpr uint32_t kFourCcBC5U = MakeFourCC('B', 'C', '5', 'U');
        constexpr uint32_t kFourCcDX10 = MakeFourCC('D', 'X', '1', '0');

        constexpr uint32_t kDdpfFourCC = 0x00000004u;
        constexpr uint32_t kDdpfRGB = 0x00000040u;
        constexpr uint32_t kDdsCaps2Cubemap = 0x00000200u;

        // Layout per the documented DDS_PIXELFORMAT (32 bytes).
        struct DdsPixelFormat
        {
            uint32_t size;
            uint32_t flags;
            uint32_t fourCC;
            uint32_t rgbBitCount;
            uint32_t rBitMask;
            uint32_t gBitMask;
            uint32_t bBitMask;
            uint32_t aBitMask;
        };
        static_assert(sizeof(DdsPixelFormat) == 32, "DdsPixelFormat must be 32 bytes");

        // Layout per the documented DDS_HEADER (124 bytes, excluding the 4-byte magic).
        struct DdsHeader
        {
            uint32_t size;
            uint32_t flags;
            uint32_t height;
            uint32_t width;
            uint32_t pitchOrLinearSize;
            uint32_t depth;
            uint32_t mipMapCount;
            uint32_t reserved1[11];
            DdsPixelFormat ddspf;
            uint32_t caps;
            uint32_t caps2;
            uint32_t caps3;
            uint32_t caps4;
            uint32_t reserved2;
        };
        static_assert(sizeof(DdsHeader) == 124, "DdsHeader must be 124 bytes");

        // Layout per the documented DDS_HEADER_DXT10 (20 bytes).
        struct DdsHeaderDxt10
        {
            uint32_t dxgiFormat;
            uint32_t resourceDimension;
            uint32_t miscFlag;
            uint32_t arraySize;
            uint32_t miscFlags2;
        };
        static_assert(sizeof(DdsHeaderDxt10) == 20, "DdsHeaderDxt10 must be 20 bytes");

        // DXGI_FORMAT values relevant to block-compressed / uncompressed 32bpp textures.
        enum DxgiFormat : uint32_t
        {
            DXGI_FORMAT_R8G8B8A8_UNORM = 28,
            DXGI_FORMAT_B8G8R8A8_UNORM = 87,
            DXGI_FORMAT_BC1_UNORM = 71,
            DXGI_FORMAT_BC1_UNORM_SRGB = 72,
            DXGI_FORMAT_BC2_UNORM = 74,
            DXGI_FORMAT_BC2_UNORM_SRGB = 75,
            DXGI_FORMAT_BC3_UNORM = 77,
            DXGI_FORMAT_BC3_UNORM_SRGB = 78,
            DXGI_FORMAT_BC4_UNORM = 80,
            DXGI_FORMAT_BC5_UNORM = 83,
            DXGI_FORMAT_BC6H_UF16 = 95,
            DXGI_FORMAT_BC6H_SF16 = 96,
            DXGI_FORMAT_BC7_UNORM = 98,
            DXGI_FORMAT_BC7_UNORM_SRGB = 99,
        };

        enum class BlockFormat
        {
            None,
            BC1,
            BC2,
            BC3,
            BC4,
            BC5,
            BC7,
            Rgba8,
            Bgra8,
        };

        std::wstring FormatFourCcForLog(uint32_t fourCC)
        {
            wchar_t buf[5] = {
                static_cast<wchar_t>(fourCC & 0xFF),
                static_cast<wchar_t>((fourCC >> 8) & 0xFF),
                static_cast<wchar_t>((fourCC >> 16) & 0xFF),
                static_cast<wchar_t>((fourCC >> 24) & 0xFF),
                L'\0'
            };
            return std::wstring(buf);
        }

        // Decodes one 4x4 block for the given format into a local 64-byte RGBA8 buffer
        // (16-byte row pitch). BC4/BC5 are decoded into smaller temp buffers first and
        // then expanded so every format shares the same RGBA8 copy-out path below.
        void DecodeBlockToRgba(BlockFormat format, const uint8_t* block, uint8_t rgba[64])
        {
            switch (format) {
                case BlockFormat::BC1:
                    bcdec_bc1(block, rgba, 16);
                    break;
                case BlockFormat::BC2:
                    bcdec_bc2(block, rgba, 16);
                    break;
                case BlockFormat::BC3:
                    bcdec_bc3(block, rgba, 16);
                    break;
                case BlockFormat::BC7:
                    bcdec_bc7(block, rgba, 16);
                    break;
                case BlockFormat::BC4: {
                    uint8_t r[16]; // 4x4, 1 byte/pixel, pitch 4
                    bcdec_bc4(block, r, 4);
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            const uint8_t v = r[y * 4 + x];
                            uint8_t* dst = &rgba[(y * 4 + x) * 4];
                            dst[0] = v;
                            dst[1] = v;
                            dst[2] = v;
                            dst[3] = 255;
                        }
                    }
                    break;
                }
                case BlockFormat::BC5: {
                    uint8_t rg[32]; // 4x4, 2 bytes/pixel (R,G interleaved), pitch 8
                    bcdec_bc5(block, rg, 8);
                    for (int y = 0; y < 4; ++y) {
                        for (int x = 0; x < 4; ++x) {
                            const uint8_t r = rg[y * 8 + x * 2 + 0];
                            const uint8_t g = rg[y * 8 + x * 2 + 1];
                            uint8_t* dst = &rgba[(y * 4 + x) * 4];
                            dst[0] = r;
                            dst[1] = g;
                            dst[2] = 255;
                            dst[3] = 255;
                        }
                    }
                    break;
                }
                default:
                    std::memset(rgba, 0, 64);
                    break;
            }
        }

        int BlockByteSize(BlockFormat format)
        {
            switch (format) {
                case BlockFormat::BC1: return BCDEC_BC1_BLOCK_SIZE;
                case BlockFormat::BC2: return BCDEC_BC2_BLOCK_SIZE;
                case BlockFormat::BC3: return BCDEC_BC3_BLOCK_SIZE;
                case BlockFormat::BC4: return BCDEC_BC4_BLOCK_SIZE;
                case BlockFormat::BC5: return BCDEC_BC5_BLOCK_SIZE;
                case BlockFormat::BC7: return BCDEC_BC7_BLOCK_SIZE;
                default: return 0;
            }
        }

        // Largest mip dimension decoded to RGBA8. Sized so a full 2K-texture scene set
        // (Bistro) stays within a sane CPU/GPU footprint; raise it if a scene needs more
        // texel density than this and the memory budget allows.
        constexpr uint32_t kMaxDecodedTextureDimension = 1024u;

        // Byte size of one mip level in its stored (compressed or raw) form, used to walk
        // the mip chain without decoding.
        size_t ComputeLevelByteSize(BlockFormat format, uint32_t width, uint32_t height, uint32_t rgbBitCount);

        bool IsBlockCompressed(BlockFormat format)
        {
            switch (format) {
                case BlockFormat::BC1:
                case BlockFormat::BC2:
                case BlockFormat::BC3:
                case BlockFormat::BC4:
                case BlockFormat::BC5:
                case BlockFormat::BC7:
                    return true;
                default:
                    return false;
            }
        }

        size_t ComputeLevelByteSize(BlockFormat format, uint32_t width, uint32_t height, uint32_t rgbBitCount)
        {
            if (IsBlockCompressed(format)) {
                const size_t blocksX = (static_cast<size_t>(width) + 3u) / 4u;
                const size_t blocksY = (static_cast<size_t>(height) + 3u) / 4u;
                return blocksX * blocksY * static_cast<size_t>(BlockByteSize(format));
            }
            const size_t bytesPerPixel = (rgbBitCount > 0u) ? (rgbBitCount / 8u) : 4u;
            return static_cast<size_t>(width) * static_cast<size_t>(height) * bytesPerPixel;
        }

        bool DecodeBlockCompressed(BlockFormat format,
                                    const uint8_t* srcData,
                                    size_t srcAvailable,
                                    UINT width,
                                    UINT height,
                                    std::vector<uint8_t>& outPixels,
                                    const std::wstring& path)
        {
            const int blockBytes = BlockByteSize(format);
            const uint32_t blocksX = (width + 3) / 4;
            const uint32_t blocksY = (height + 3) / 4;
            const size_t requiredBytes = static_cast<size_t>(blocksX) * static_cast<size_t>(blocksY) * static_cast<size_t>(blockBytes);
            if (requiredBytes == 0 || requiredBytes > srcAvailable) {
                DebugLog((L"[DdsTextureLoader] Truncated block-compressed data in \"" + path + L"\"").c_str());
                return false;
            }

            outPixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);

            const uint8_t* srcCursor = srcData;
            for (uint32_t by = 0; by < blocksY; ++by) {
                for (uint32_t bx = 0; bx < blocksX; ++bx) {
                    uint8_t rgba[64];
                    DecodeBlockToRgba(format, srcCursor, rgba);
                    srcCursor += blockBytes;

                    const uint32_t originX = bx * 4;
                    const uint32_t originY = by * 4;
                    const uint32_t copyW = std::min<uint32_t>(4u, width - originX);
                    const uint32_t copyH = std::min<uint32_t>(4u, height - originY);
                    for (uint32_t y = 0; y < copyH; ++y) {
                        const uint8_t* srcRow = &rgba[y * 16];
                        uint8_t* dstRow = &outPixels[(static_cast<size_t>(originY + y) * width + originX) * 4];
                        std::memcpy(dstRow, srcRow, static_cast<size_t>(copyW) * 4u);
                    }
                }
            }

            return true;
        }

        bool DecodeUncompressed32Bpp(BlockFormat format,
                                      const uint8_t* srcData,
                                      size_t srcAvailable,
                                      UINT width,
                                      UINT height,
                                      uint32_t rowPitch,
                                      std::vector<uint8_t>& outPixels,
                                      const std::wstring& path)
        {
            const uint32_t pitch = (rowPitch != 0) ? rowPitch : (width * 4u);
            const size_t requiredBytes = static_cast<size_t>(pitch) * static_cast<size_t>(height);
            if (requiredBytes == 0 || requiredBytes > srcAvailable) {
                DebugLog((L"[DdsTextureLoader] Truncated uncompressed pixel data in \"" + path + L"\"").c_str());
                return false;
            }

            outPixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
            for (uint32_t y = 0; y < height; ++y) {
                const uint8_t* srcRow = srcData + static_cast<size_t>(y) * pitch;
                uint8_t* dstRow = &outPixels[static_cast<size_t>(y) * width * 4u];
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t* s = srcRow + static_cast<size_t>(x) * 4u;
                    uint8_t* d = dstRow + static_cast<size_t>(x) * 4u;
                    if (format == BlockFormat::Bgra8) {
                        d[0] = s[2];
                        d[1] = s[1];
                        d[2] = s[0];
                        d[3] = s[3];
                    } else {
                        d[0] = s[0];
                        d[1] = s[1];
                        d[2] = s[2];
                        d[3] = s[3];
                    }
                }
            }

            return true;
        }
    }

    namespace DdsTextureLoader
    {
        bool LoadRgba8FromDds(const std::wstring& path,
                              std::vector<uint8_t>& outPixels,
                              UINT& outWidth,
                              UINT& outHeight)
        {
            outPixels.clear();
            outWidth = 0;
            outHeight = 0;

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                DebugLog((L"[DdsTextureLoader] Failed to open \"" + path + L"\"").c_str());
                return false;
            }

            const std::streamoff fileSizeOff = file.tellg();
            if (fileSizeOff < 0) {
                DebugLog((L"[DdsTextureLoader] Failed to determine size of \"" + path + L"\"").c_str());
                return false;
            }
            const size_t fileSize = static_cast<size_t>(fileSizeOff);
            file.seekg(0, std::ios::beg);

            constexpr size_t kMinimumHeaderSize = 4 + sizeof(DdsHeader);
            if (fileSize < kMinimumHeaderSize) {
                DebugLog((L"[DdsTextureLoader] File too small to be a valid DDS: \"" + path + L"\"").c_str());
                return false;
            }

            std::vector<uint8_t> fileBytes(fileSize);
            if (!file.read(reinterpret_cast<char*>(fileBytes.data()), static_cast<std::streamsize>(fileSize))) {
                DebugLog((L"[DdsTextureLoader] Failed to read \"" + path + L"\"").c_str());
                return false;
            }

            size_t cursor = 0;
            uint32_t magic = 0;
            std::memcpy(&magic, fileBytes.data() + cursor, sizeof(magic));
            cursor += sizeof(magic);
            if (magic != kDdsMagic) {
                DebugLog((L"[DdsTextureLoader] Missing 'DDS ' magic in \"" + path + L"\"").c_str());
                return false;
            }

            DdsHeader header{};
            std::memcpy(&header, fileBytes.data() + cursor, sizeof(header));
            cursor += sizeof(header);

            if (header.size != sizeof(DdsHeader)) {
                DebugLog((L"[DdsTextureLoader] Unexpected DDS_HEADER size in \"" + path + L"\"").c_str());
                return false;
            }

            if (header.depth > 1) {
                DebugLog((L"[DdsTextureLoader] Volume (3D) DDS textures are not supported: \"" + path + L"\"").c_str());
                return false;
            }
            if (header.caps2 & kDdsCaps2Cubemap) {
                DebugLog((L"[DdsTextureLoader] Cubemap DDS textures are not supported by this loader: \"" + path + L"\"").c_str());
                return false;
            }

            // Not const: the mip-chain walk below may advance these to a smaller level.
            UINT width = header.width;
            UINT height = header.height;
            if (width == 0 || height == 0) {
                DebugLog((L"[DdsTextureLoader] Invalid dimensions in \"" + path + L"\"").c_str());
                return false;
            }

            BlockFormat format = BlockFormat::None;
            bool isSrgbHint = false; // Parsed for completeness; sRGB conversion is handled by the consumer, not here.
            (void)isSrgbHint;

            if ((header.ddspf.flags & kDdpfFourCC) && header.ddspf.fourCC == kFourCcDX10) {
                constexpr size_t kDxt10HeaderSize = sizeof(DdsHeaderDxt10);
                if (fileSize < cursor + kDxt10HeaderSize) {
                    DebugLog((L"[DdsTextureLoader] Truncated DX10 header in \"" + path + L"\"").c_str());
                    return false;
                }
                DdsHeaderDxt10 dxt10{};
                std::memcpy(&dxt10, fileBytes.data() + cursor, kDxt10HeaderSize);
                cursor += kDxt10HeaderSize;

                switch (dxt10.dxgiFormat) {
                    case DXGI_FORMAT_BC1_UNORM:
                    case DXGI_FORMAT_BC1_UNORM_SRGB:
                        format = BlockFormat::BC1;
                        break;
                    case DXGI_FORMAT_BC2_UNORM:
                    case DXGI_FORMAT_BC2_UNORM_SRGB:
                        format = BlockFormat::BC2;
                        break;
                    case DXGI_FORMAT_BC3_UNORM:
                    case DXGI_FORMAT_BC3_UNORM_SRGB:
                        format = BlockFormat::BC3;
                        break;
                    case DXGI_FORMAT_BC4_UNORM:
                        format = BlockFormat::BC4;
                        break;
                    case DXGI_FORMAT_BC5_UNORM:
                        format = BlockFormat::BC5;
                        break;
                    case DXGI_FORMAT_BC7_UNORM:
                    case DXGI_FORMAT_BC7_UNORM_SRGB:
                        format = BlockFormat::BC7;
                        break;
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                        format = BlockFormat::Rgba8;
                        break;
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                        format = BlockFormat::Bgra8;
                        break;
                    case DXGI_FORMAT_BC6H_UF16:
                    case DXGI_FORMAT_BC6H_SF16:
                        DebugLog((L"[DdsTextureLoader] BC6H (HDR) is not supported: \"" + path + L"\" DXGI_FORMAT=" +
                                  std::to_wstring(dxt10.dxgiFormat)).c_str());
                        return false;
                    default:
                        DebugLog((L"[DdsTextureLoader] Unsupported DXGI_FORMAT " + std::to_wstring(dxt10.dxgiFormat) +
                                  L" in \"" + path + L"\"").c_str());
                        return false;
                }
            } else if (header.ddspf.flags & kDdpfFourCC) {
                const uint32_t fourCC = header.ddspf.fourCC;
                if (fourCC == kFourCcDXT1) {
                    format = BlockFormat::BC1;
                } else if (fourCC == kFourCcDXT2 || fourCC == kFourCcDXT3) {
                    format = BlockFormat::BC2;
                } else if (fourCC == kFourCcDXT4 || fourCC == kFourCcDXT5) {
                    format = BlockFormat::BC3;
                } else if (fourCC == kFourCcATI1 || fourCC == kFourCcBC4U) {
                    format = BlockFormat::BC4;
                } else if (fourCC == kFourCcATI2 || fourCC == kFourCcBC5U) {
                    format = BlockFormat::BC5;
                } else {
                    DebugLog((L"[DdsTextureLoader] Unsupported FourCC '" + FormatFourCcForLog(fourCC) +
                              L"' in \"" + path + L"\"").c_str());
                    return false;
                }
            } else if ((header.ddspf.flags & kDdpfRGB) && header.ddspf.rgbBitCount == 32) {
                if (header.ddspf.rBitMask == 0x000000FFu) {
                    format = BlockFormat::Rgba8;
                } else if (header.ddspf.rBitMask == 0x00FF0000u) {
                    format = BlockFormat::Bgra8;
                } else {
                    DebugLog((L"[DdsTextureLoader] Unrecognized 32bpp channel mask layout in \"" + path + L"\"").c_str());
                    return false;
                }
            } else {
                DebugLog((L"[DdsTextureLoader] Unsupported pixel format (no FourCC, not 32bpp RGB) in \"" + path + L"\"").c_str());
                return false;
            }

            // Skip to a mip that fits the streaming budget instead of always decoding the
            // top level. Bistro ships 337 textures at 2048x2048; decoding every top mip to
            // RGBA8 costs 5.3 GB of CPU memory and as much again in upload buffers, which
            // exhausts the GPU before the scene can render. Decoding a smaller mip keeps
            // the whole set well inside budget, and DDS already carries the chain so this
            // costs nothing but an offset walk.
            const uint32_t mipCount = (header.mipMapCount > 0u) ? header.mipMapCount : 1u;
            uint32_t mipLevel = 0u;
            while (mipLevel + 1u < mipCount &&
                   (width > kMaxDecodedTextureDimension || height > kMaxDecodedTextureDimension)) {
                const size_t levelBytes = ComputeLevelByteSize(format, width, height, header.ddspf.rgbBitCount);
                if (levelBytes == 0u || cursor + levelBytes > fileSize) {
                    break; // Truncated chain: decode what we are already positioned at.
                }
                cursor += levelBytes;
                width = (width > 1u) ? width / 2u : 1u;
                height = (height > 1u) ? height / 2u : 1u;
                ++mipLevel;
            }

            const uint8_t* pixelData = fileBytes.data() + cursor;
            const size_t pixelAvailable = fileSize - cursor;

            bool ok = false;
            if (IsBlockCompressed(format)) {
                ok = DecodeBlockCompressed(format, pixelData, pixelAvailable, width, height, outPixels, path);
            } else {
                ok = DecodeUncompressed32Bpp(format, pixelData, pixelAvailable, width, height,
                                              header.pitchOrLinearSize, outPixels, path);
            }

            if (!ok) {
                outPixels.clear();
                return false;
            }

            outWidth = width;
            outHeight = height;
            return true;
        }
    }
}
