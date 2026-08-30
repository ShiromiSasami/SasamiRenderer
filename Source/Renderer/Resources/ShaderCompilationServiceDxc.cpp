#include "Renderer/Resources/ShaderCompilationService.h"

#include "Renderer/Utilities/AdHocShaderCompileUtility.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace SasamiRenderer::ShaderCompilationService
{
    // Stateless aside from filesystem I/O: source-root resolution, cache-path derivation
    // and staleness checks are all pure functions of the file system. Concurrent duplicate
    // compiles of the same shader from multiple threads are benign: each writes its own
    // temporary and renames it into place, so the cache ends up holding one whole blob
    // (last writer wins) rather than an interleaved one. Callers that need to avoid
    // redundant compiles entirely should add a blob cache above this layer.
    bool GetOrCompileShaderBytecodeDxc(const char* logTag,
                                       const wchar_t* relPath,
                                       const char* entry,
                                       const char* profile,
                                       std::vector<uint8_t>& outBytecode)
    {
        const std::filesystem::path sourcePath = GetShaderSourceRoot() / relPath;
        const char* cacheEntry = (entry && entry[0] != '\0') ? entry : "lib";
        const std::filesystem::path compiledPath = ResolveCompiledShaderPath(sourcePath, cacheEntry, profile);

        if (IsCompiledShaderUpToDate(compiledPath, sourcePath)) {
            std::ifstream input(compiledPath, std::ios::binary | std::ios::ate);
            if (input) {
                const std::streamsize size = input.tellg();
                if (size > 0) {
                    outBytecode.resize(static_cast<size_t>(size));
                    input.seekg(0);
                    if (input.read(reinterpret_cast<char*>(outBytecode.data()), size)) {
                        LogShaderResolveMessage(sourcePath, cacheEntry, profile, "loaded precompiled shader", compiledPath);
                        return true;
                    }
                }
            }

            LogShaderResolveMessage(sourcePath, cacheEntry, profile,
                                    "failed to read precompiled shader, falling back to runtime compile", compiledPath);
        } else {
            LogShaderResolveMessage(sourcePath, cacheEntry, profile,
                                    "precompiled shader missing or stale, runtime compiling", compiledPath);
        }

        outBytecode.clear();
        if (!AdHocShaderCompileUtility::CompileShader(logTag, relPath, entry, profile, outBytecode)) {
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(compiledPath.parent_path(), ec);
        if (ec) {
            LogShaderResolveMessage(sourcePath, cacheEntry, profile,
                                    "cache directory creation failed; keeping runtime blob only", compiledPath);
            return true;
        }

        // Write to a per-thread temporary and rename, matching StaticMeshCache::Save. Two
        // worker threads compiling the same shader would otherwise interleave into one
        // truncated .cso, and IsCompiledShaderUpToDate() only compares timestamps -- a torn
        // blob would be read back as valid bytecode on the next run.
        std::filesystem::path tempPath = compiledPath;
        tempPath += ".tmp." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));

        bool written = false;
        {
            std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
            if (output) {
                output.write(reinterpret_cast<const char*>(outBytecode.data()),
                             static_cast<std::streamsize>(outBytecode.size()));
                // Flush explicitly: a failure inside the destructor's flush would otherwise
                // go unnoticed and rename a short blob into the cache.
                output.flush();
                written = static_cast<bool>(output);
            }
        }

        if (written) {
            ec.clear();
            std::filesystem::remove(compiledPath, ec);
            ec.clear();
            std::filesystem::rename(tempPath, compiledPath, ec);
            written = !ec;
        }

        if (written) {
            LogShaderResolveMessage(sourcePath, cacheEntry, profile, "runtime compiled shader and updated cache", compiledPath);
        } else {
            std::error_code cleanupEc;
            std::filesystem::remove(tempPath, cleanupEc);
            LogShaderResolveMessage(sourcePath, cacheEntry, profile, "compiled shader could not be written", compiledPath);
        }

        return true;
    }
} // namespace SasamiRenderer::ShaderCompilationService
