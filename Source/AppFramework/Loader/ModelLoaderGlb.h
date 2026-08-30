#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace SasamiRenderer
{
    // Parses a binary glTF (.glb) container: 12-byte header followed by a
    // mandatory JSON chunk and an optional BIN chunk (glTF 2.0 spec,
    // "Binary glTF Layout"). outBin is left empty if the file has no BIN chunk
    // (buffers with no "uri" in the JSON then refer to this embedded chunk).
    bool ParseGlbContainer(const std::vector<uint8_t>& fileBytes, std::string& outJson, std::vector<uint8_t>& outBin);
}
