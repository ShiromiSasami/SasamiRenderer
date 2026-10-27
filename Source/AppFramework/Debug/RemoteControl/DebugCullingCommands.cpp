#include "AppFramework/Debug/RemoteControl/DebugCullingCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "Renderer/Culling/GpuHzbSelfTest.h"
#include "Renderer/Culling/HzbPyramid.h"
#include "Renderer/Culling/HzbSelfTest.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Runtime/Renderer.h"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        bool ParseDimension(const std::string& text, uint32_t& out)
        {
            try {
                size_t consumed = 0;
                const unsigned long value = std::stoul(text, &consumed);
                if (consumed != text.size() || value < 1 || value > 8192)
                    return false;

                out = static_cast<uint32_t>(value);
                return true;
            }
            catch (...) {
                return false;
            }
        }

        std::string RunBench(uint32_t width, uint32_t height)
        {
            std::vector<float> depth(static_cast<size_t>(width) * height);
            uint32_t state = 12345u;
            for (float& d : depth) {
                state = state * 1664525u + 1013904223u;
                d = static_cast<float>(state >> 8) / 16777216.0f;
            }

            Render::Culling::HzbPyramid pyramid;
            const auto begin = std::chrono::steady_clock::now();
            pyramid.Build(depth.data(), width, height);
            const auto end = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(end - begin).count();

            std::ostringstream out;
            out << "OK width=" << width << " height=" << height << " levels=" << pyramid.LevelCount()
                << " ms=" << ms << " bytes=" << pyramid.ByteSize();
            return out.str();
        }
    }

    void RegisterCullingCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("hzb", "selftest | gpu | bench <width> <height> - CPU reverse-Z HZB pyramid reference (min-depth, NPOT floor convention); gpu: GPU build vs CPU bit identity, blocks until GPU idle",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() == 1 && args[0] == "selftest")
                    return Render::Culling::RunHzbSelfTest();

                if (args.size() == 1 && args[0] == "gpu") {
                    IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                    if (!device)
                        return "ERR no RHI device";

                    return Render::Culling::RunGpuHzbSelfTest(*device);
                }

                if (args.size() == 3 && args[0] == "bench") {
                    uint32_t width = 0;
                    uint32_t height = 0;
                    if (!ParseDimension(args[1], width) || !ParseDimension(args[2], height))
                        return "ERR width and height must be integers in 1..8192";

                    return RunBench(width, height);
                }

                return "ERR usage: hzb selftest | hzb gpu | hzb bench <width> <height>";
            });
    }
}
