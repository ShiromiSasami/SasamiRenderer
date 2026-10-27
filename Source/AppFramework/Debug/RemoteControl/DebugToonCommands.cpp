#include "AppFramework/Debug/RemoteControl/DebugToonCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Component/CpuTextureLoader.h"
#include "AppFramework/Object/StaticModel.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Toon/FaceShadowSelfTest.h"
#include "Renderer/Toon/FaceShadowThresholdMap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        using ToonSettings = Render::LightSystem::ToonSettings;

        std::optional<float> ParseFiniteFloat(const std::string& text)
        {
            try {
                size_t consumed = 0;
                const float value = std::stof(text, &consumed);
                if (consumed != text.size() || !std::isfinite(value)) {
                    return std::nullopt;
                }
                return value;
            }
            catch (...) {
                return std::nullopt;
            }
        }

        std::optional<size_t> ParseIndex(const std::string& text)
        {
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
                return std::nullopt;
            }
            try {
                return static_cast<size_t>(std::stoull(text));
            }
            catch (...) {
                return std::nullopt;
            }
        }

        // Comma-separated indices, each either "i" or an inclusive range "a..b" (a <= b).
        bool ParseMeshList(const std::string& text, std::vector<size_t>& out)
        {
            constexpr size_t MAX_RANGE = 65536;
            std::vector<size_t> list;
            std::istringstream stream(text);
            std::string item;
            while (std::getline(stream, item, ',')) {
                const size_t dots = item.find("..");
                const std::optional<size_t> first = ParseIndex(item.substr(0, dots));
                const std::optional<size_t> last =
                    (dots == std::string::npos) ? first : ParseIndex(item.substr(dots + 2));
                if (!first || !last || *first > *last || *last - *first >= MAX_RANGE) {
                    return false;
                }
                for (size_t index = *first; index <= *last; ++index) {
                    list.push_back(index);
                }
            }
            if (list.empty()) {
                return false;
            }
            out = std::move(list);
            return true;
        }

        bool ParseColor3(const std::string& text, float (&out)[3])
        {
            float values[3] = {};
            size_t begin = 0;
            for (int i = 0; i < 3; ++i) {
                const size_t end = (i < 2) ? text.find(',', begin) : text.size();
                if (end == std::string::npos) {
                    return false;
                }
                const std::optional<float> value = ParseFiniteFloat(text.substr(begin, end - begin));
                if (!value) {
                    return false;
                }
                values[i] = *value;
                begin = end + 1;
            }
            out[0] = values[0];
            out[1] = values[1];
            out[2] = values[2];
            return true;
        }

        using ToonFaceSettings = Render::LightSystem::ToonFaceSettings;
        using ToonHairSettings = Render::LightSystem::ToonHairSettings;
        using ToonHairShadowSettings = Render::LightSystem::ToonHairShadowSettings;
        using ToonHeadNormalSettings = Render::LightSystem::ToonHeadNormalSettings;

        std::string FormatFaceSettings(const ToonFaceSettings& s, bool hasMap)
        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "enabled=" << (s.enabled ? 1 : 0)
                << " forward=" << s.forward[0] << ',' << s.forward[1] << ',' << s.forward[2]
                << " right=" << s.right[0] << ',' << s.right[1] << ',' << s.right[2]
                << " soft=" << s.softness
                << " map=" << (hasMap ? "set" : "none");
            return out.str();
        }

        // Test map: nested half-planes merged by the SDF builder, so the threshold rises with u and the
        // lit region grows across the texture as the light moves to the front. Packed into R (rgb), a=255.
        bool BuildSyntheticFaceMap(uint32_t size, uint32_t maskCount, CpuTextureRgba8& out, std::string& error)
        {
            std::vector<Render::Toon::FaceShadowMask> masks(maskCount);
            for (uint32_t k = 0; k < maskCount; ++k) {
                Render::Toon::FaceShadowMask& mask = masks[k];
                mask.width = size;
                mask.height = size;
                mask.lit.assign(static_cast<size_t>(size) * size, 0);
                const float edge = static_cast<float>(size) * static_cast<float>(k + 1) / static_cast<float>(maskCount + 1);
                for (uint32_t y = 0; y < size; ++y) {
                    for (uint32_t x = 0; x < size; ++x) {
                        mask.lit[static_cast<size_t>(y) * size + x] = static_cast<float>(x) < edge ? 1 : 0;
                    }
                }
            }
            std::vector<float> threshold;
            if (!Render::Toon::BuildFaceShadowThresholdMap(masks, threshold, &error)) {
                return false;
            }
            out.width = size;
            out.height = size;
            out.pixels.resize(static_cast<size_t>(size) * size * 4);
            for (size_t i = 0; i < threshold.size(); ++i) {
                const float v = std::clamp(threshold[i], 0.0f, 1.0f);
                const uint8_t byte = static_cast<uint8_t>(std::lround(v * 255.0f));
                out.pixels[i * 4 + 0] = byte;
                out.pixels[i * 4 + 1] = byte;
                out.pixels[i * 4 + 2] = byte;
                out.pixels[i * 4 + 3] = 255;
            }
            return true;
        }

        std::string FormatSettings(const ToonSettings& s)
        {
            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "bandCount=" << s.bandCount
                << " bandSoftness=" << s.bandSoftness
                << " specularThreshold=" << s.specularThreshold
                << " specularSoftness=" << s.specularSoftness
                << " rimStrength=" << s.rimStrength
                << " rimPower=" << s.rimPower
                << " specularIntensity=" << s.specularIntensity
                << " shadowFloor=" << s.shadowFloor
                << " ambientScale=" << s.ambientScale
                << " outlineWidth=" << s.outlineWidth
                << " outlineColor=" << s.outlineColor[0] << ',' << s.outlineColor[1] << ',' << s.outlineColor[2]
                << " depthRimWidth=" << s.depthRimWidth
                << " depthRimThreshold=" << s.depthRimThreshold
                << " depthRimStrength=" << s.depthRimStrength;
            return out.str();
        }

        float* FindScalarField(ToonSettings& s, const std::string& key)
        {
            if (key == "bandCount") return &s.bandCount;
            if (key == "bandSoftness") return &s.bandSoftness;
            if (key == "specularThreshold") return &s.specularThreshold;
            if (key == "specularSoftness") return &s.specularSoftness;
            if (key == "rimStrength") return &s.rimStrength;
            if (key == "rimPower") return &s.rimPower;
            if (key == "specularIntensity") return &s.specularIntensity;
            if (key == "shadowFloor") return &s.shadowFloor;
            if (key == "ambientScale") return &s.ambientScale;
            if (key == "outlineWidth") return &s.outlineWidth;
            if (key == "depthRimWidth") return &s.depthRimWidth;
            if (key == "depthRimThreshold") return &s.depthRimThreshold;
            if (key == "depthRimStrength") return &s.depthRimStrength;
            return nullptr;
        }

        void SetOutlineColor(ToonSettings& s, float r, float g, float b)
        {
            s.outlineColor[0] = r;
            s.outlineColor[1] = g;
            s.outlineColor[2] = b;
        }

        std::optional<ToonSettings> MakePreset(const std::string& name)
        {
            ToonSettings s{};
            if (name == "default") {
                return s;
            }
            if (name == "anime") {
                s.bandCount = 2.0f;
                s.bandSoftness = 0.02f;
                s.shadowFloor = 0.45f;
                s.specularThreshold = 0.92f;
                s.specularIntensity = 0.3f;
                s.rimStrength = 0.15f;
                s.rimPower = 4.0f;
                s.ambientScale = 0.45f;
                s.outlineWidth = 0.015f;
                SetOutlineColor(s, 0.12f, 0.08f, 0.10f);
                s.depthRimWidth = 12.0f;
                s.depthRimThreshold = 0.3f;
                s.depthRimStrength = 0.5f;
                return s;
            }
            if (name == "ggxrd") {
                s.bandCount = 2.0f;
                s.bandSoftness = 0.005f;
                s.shadowFloor = 0.35f;
                s.specularThreshold = 0.95f;
                s.specularIntensity = 0.15f;
                s.rimStrength = 0.0f;
                s.ambientScale = 0.3f;
                s.outlineWidth = 0.025f;
                SetOutlineColor(s, 0.05f, 0.03f, 0.04f);
                s.depthRimWidth = 0.0f;
                return s;
            }
            return std::nullopt;
        }
    }

    void RegisterToonCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("toon", "[key=value ...] | preset <default|anime|ggxrd> - print or set the global cel shading settings",
            [&app](const std::vector<std::string>& args) -> std::string {
                Render::Renderer& renderer = app.GetRenderer();
                ToonSettings settings = renderer.GetToonSettings();

                if (args.empty()) {
                    return "OK " + FormatSettings(settings);
                }

                if (args[0] == "preset") {
                    if (args.size() != 2) {
                        return "ERR usage: toon preset <default|anime|ggxrd>";
                    }
                    const std::optional<ToonSettings> preset = MakePreset(args[1]);
                    if (!preset) {
                        return "ERR unknown preset '" + args[1] + "' (default|anime|ggxrd)";
                    }
                    renderer.SetToonSettings(*preset);
                    return "OK preset=" + args[1] + " " + FormatSettings(renderer.GetToonSettings());
                }

                for (const std::string& arg : args) {
                    const size_t eq = arg.find('=');
                    if (eq == std::string::npos || eq == 0) {
                        return "ERR expected key=value, got '" + arg + "'";
                    }
                    const std::string key = arg.substr(0, eq);
                    const std::string valueText = arg.substr(eq + 1);

                    if (key == "outlineColor") {
                        if (!ParseColor3(valueText, settings.outlineColor)) {
                            return "ERR outlineColor expects r,g,b numbers, got '" + valueText + "'";
                        }
                        continue;
                    }

                    float* field = FindScalarField(settings, key);
                    if (!field) {
                        return "ERR unknown key '" + key + "'";
                    }
                    const std::optional<float> value = ParseFiniteFloat(valueText);
                    if (!value) {
                        return "ERR not a number: '" + valueText + "' for " + key;
                    }
                    *field = *value;
                }

                renderer.SetToonSettings(settings);
                return "OK " + FormatSettings(renderer.GetToonSettings());
            });

        registry.Register("toon.apply", "[all|off] - switch every mesh of every static model to ShadingModel::Toon (all, default) or back to Standard (off), and make sure the outline pass is inserted",
            [&app](const std::vector<std::string>& args) -> std::string {
                bool toon = true;
                if (!args.empty()) {
                    if (args[0] == "all") {
                        toon = true;
                    }
                    else if (args[0] == "off") {
                        toon = false;
                    }
                    else {
                        return "ERR usage: toon.apply [all|off]";
                    }
                }

                const Render::ShadingModel target = toon ? Render::ShadingModel::Toon : Render::ShadingModel::Standard;
                size_t modelCount = 0;
                size_t meshCount = 0;
                for (StaticModel* model : app.GetStaticModels()) {
                    if (!model) {
                        continue;
                    }
                    ++modelCount;
                    for (size_t meshIndex = 0;; ++meshIndex) {
                        const Render::SurfaceMaterial* current = model->GetMaterial(meshIndex);
                        if (!current) {
                            break;
                        }
                        Render::SurfaceMaterial material = *current;
                        material.shadingModel = target;
                        if (model->SetMaterial(meshIndex, material)) {
                            ++meshCount;
                        }
                    }
                }
                // SetMaterial only edits the component; proxies are rebuilt from it on the next sync.
                app.InvalidateRenderObjects();

                // Idempotent: the preset rebuilds the pass sequence from the default path each call.
                if (toon) {
                    app.UseToonRenderNodePreset();
                }

                std::ostringstream out;
                out << "OK models=" << modelCount << " meshes=" << meshCount
                    << " mode=" << (toon ? "toon" : "standard");
                return out.str();
            });

        registry.Register("toon.facemap", "selftest | synth <size 8..2048> <masks 2..32> - build the SDF face-shadow threshold map (CPU) from synthetic nested masks and report timing/range",
            [](const std::vector<std::string>& args) -> std::string {
                if (args.size() == 1 && args[0] == "selftest") {
                    return Render::Toon::RunFaceShadowSelfTest();
                }
                if (args.size() != 3 || args[0] != "synth") {
                    return "ERR usage: toon.facemap selftest | synth <size 8..2048> <masks 2..32>";
                }
                int size = 0;
                int maskCount = 0;
                try {
                    size = std::stoi(args[1]);
                    maskCount = std::stoi(args[2]);
                }
                catch (...) {
                    return "ERR size and masks must be integers";
                }
                if (size < 8 || size > 2048 || maskCount < 2 || maskCount > 32) {
                    return "ERR size must be 8..2048 and masks 2..32";
                }

                // Nested by construction: a growing half-plane united with a growing disc.
                std::vector<Render::Toon::FaceShadowMask> masks(static_cast<size_t>(maskCount));
                const float radiusStep = 0.4f * static_cast<float>(size) / static_cast<float>(maskCount);
                for (int k = 0; k < maskCount; ++k) {
                    Render::Toon::FaceShadowMask& mask = masks[static_cast<size_t>(k)];
                    mask.width = static_cast<uint32_t>(size);
                    mask.height = static_cast<uint32_t>(size);
                    mask.lit.assign(static_cast<size_t>(size) * size, 0);
                    const float edge = static_cast<float>(size) * 0.5f * static_cast<float>(k) / static_cast<float>(maskCount);
                    const float radius = radiusStep * static_cast<float>(k);
                    const float cx = static_cast<float>(size) * 0.7f;
                    const float cy = static_cast<float>(size) * 0.4f;
                    for (int y = 0; y < size; ++y) {
                        for (int x = 0; x < size; ++x) {
                            const float dx = static_cast<float>(x) - cx;
                            const float dy = static_cast<float>(y) - cy;
                            const bool lit = static_cast<float>(x) < edge || dx * dx + dy * dy < radius * radius;
                            mask.lit[static_cast<size_t>(y) * size + x] = lit ? 1 : 0;
                        }
                    }
                }

                std::vector<float> threshold;
                std::string error;
                const auto start = std::chrono::steady_clock::now();
                const bool built = Render::Toon::BuildFaceShadowThresholdMap(masks, threshold, &error);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (!built) {
                    return "ERR " + error;
                }

                float minValue = threshold.front();
                float maxValue = threshold.front();
                double sum = 0.0;
                for (float value : threshold) {
                    minValue = (std::min)(minValue, value);
                    maxValue = (std::max)(maxValue, value);
                    sum += value;
                }
                std::ostringstream out;
                out << std::fixed << std::setprecision(4);
                out << "OK size=" << size << " masks=" << maskCount << " ms=" << ms
                    << " min=" << minValue << " max=" << maxValue << " mean=" << sum / static_cast<double>(threshold.size());
                return out.str();
            });
        registry.Register("toon.face", "[list | on|off | mesh=<i,..>|all | model=<i> | map=<png>|synth | forward=x,y,z | right=x,y,z | soft=<f>] - SDF face shadow: pick the face meshes, the threshold map and the head frame",
            [&app](const std::vector<std::string>& args) -> std::string {
                Render::Renderer& renderer = app.GetRenderer();
                ToonFaceSettings settings = renderer.GetToonFaceSettings();
                const std::vector<StaticModel*>& models = app.GetStaticModels();

                if (!args.empty() && args[0] == "list") {
                    // Material names are not kept, so meshes are picked by index.
                    std::ostringstream out;
                    out << std::fixed << std::setprecision(3) << "OK";
                    for (size_t modelIndex = 0; modelIndex < models.size(); ++modelIndex) {
                        if (!models[modelIndex]) {
                            continue;
                        }
                        for (size_t meshIndex = 0;; ++meshIndex) {
                            const Render::SurfaceMaterial* material = models[modelIndex]->GetMaterial(meshIndex);
                            if (!material) {
                                break;
                            }
                            out << "\n model=" << modelIndex << " mesh=" << meshIndex
                                << " baseColor=" << material->baseColor[0] << ',' << material->baseColor[1] << ','
                                << material->baseColor[2] << ',' << material->baseColor[3]
                                << " alpha=" << static_cast<int>(material->alphaMode)
                                << " toon=" << (material->shadingModel == Render::ShadingModel::Toon ? 1 : 0)
                                << " face=" << (material->toonFaceShadow ? 1 : 0)
                                << " hair=" << (material->toonHair ? 1 : 0);
                        }
                    }
                    return out.str();
                }

                size_t modelIndex = 0;
                std::optional<std::vector<size_t>> meshes;
                bool allMeshes = false;
                for (const std::string& arg : args) {
                    if (arg == "on" || arg == "off") {
                        settings.enabled = (arg == "on");
                        continue;
                    }
                    const size_t eq = arg.find('=');
                    if (eq == std::string::npos) {
                        return "ERR unknown argument '" + arg + "'";
                    }
                    const std::string key = arg.substr(0, eq);
                    const std::string value = arg.substr(eq + 1);
                    if (key == "forward" || key == "right") {
                        float vec[3] = {};
                        if (!ParseColor3(value, vec)) {
                            return "ERR " + key + " must be x,y,z";
                        }
                        float* target = key == "forward" ? settings.forward : settings.right;
                        std::copy(vec, vec + 3, target);
                    }
                    else if (key == "soft") {
                        const std::optional<float> soft = ParseFiniteFloat(value);
                        if (!soft || *soft < 0.0f || *soft > 0.5f) {
                            return "ERR soft must be 0..0.5";
                        }
                        settings.softness = *soft;
                    }
                    else if (key == "model") {
                        try { modelIndex = static_cast<size_t>(std::stoul(value)); }
                        catch (...) { return "ERR model must be an index"; }
                    }
                    else if (key == "mesh") {
                        if (value == "all") {
                            allMeshes = true;
                            continue;
                        }
                        std::vector<size_t> list;
                        if (!ParseMeshList(value, list)) {
                            return "ERR mesh must be all or a list like 0,7,8 or 18..64";
                        }
                        meshes = std::move(list);
                    }
                    else if (key == "map") {
                        CpuTextureRgba8 map;
                        if (value == "synth") {
                            std::string error;
                            if (!BuildSyntheticFaceMap(256, 8, map, error)) {
                                return "ERR synth: " + error;
                            }
                        }
                        else {
                            const std::shared_ptr<const CpuTextureRgba8> loaded = LoadCpuTextureFromPath(value);
                            if (!loaded || loaded->pixels.empty()) {
                                return "ERR could not load " + value;
                            }
                            map = *loaded;
                        }
                        if (!renderer.SetToonFaceShadowMap(map)) {
                            return "ERR face map upload failed";
                        }
                    }
                    else {
                        return "ERR unknown key '" + key + "'";
                    }
                }

                size_t flagged = 0;
                if (meshes || allMeshes) {
                    if (modelIndex >= models.size() || !models[modelIndex]) {
                        return "ERR model index out of range";
                    }
                    StaticModel* model = models[modelIndex];
                    for (size_t meshIndex = 0;; ++meshIndex) {
                        const Render::SurfaceMaterial* current = model->GetMaterial(meshIndex);
                        if (!current) {
                            break;
                        }
                        const bool face = allMeshes ||
                            std::find(meshes->begin(), meshes->end(), meshIndex) != meshes->end();
                        Render::SurfaceMaterial material = *current;
                        material.toonFaceShadow = face;
                        if (model->SetMaterial(meshIndex, material) && face) {
                            ++flagged;
                        }
                    }
                    // Proxies are rebuilt from the components on the next sync.
                    app.InvalidateRenderObjects();
                }

                renderer.SetToonFaceSettings(settings);
                std::ostringstream out;
                out << "OK " << FormatFaceSettings(settings, renderer.HasToonFaceShadowMap());
                if (meshes || allMeshes) {
                    out << " faceMeshes=" << flagged;
                }
                return out.str();
            });
        registry.Register("toon.hair", "[on|off | mesh=<i,..>|all | model=<i> | up=x,y,z | shift=p,s | width=p,s | strength=p,s | soft=<f>] - toon hair angel ring: pick the hair meshes and tune the two Kajiya-Kay bands",
            [&app](const std::vector<std::string>& args) -> std::string {
                Render::Renderer& renderer = app.GetRenderer();
                ToonHairSettings settings = renderer.GetToonHairSettings();
                const std::vector<StaticModel*>& models = app.GetStaticModels();

                // "p,s" -> primary, secondary.
                auto parsePair = [](const std::string& text, float& primary, float& secondary) {
                    const size_t comma = text.find(',');
                    if (comma == std::string::npos) {
                        return false;
                    }
                    const std::optional<float> p = ParseFiniteFloat(text.substr(0, comma));
                    const std::optional<float> s = ParseFiniteFloat(text.substr(comma + 1));
                    if (!p || !s) {
                        return false;
                    }
                    primary = *p;
                    secondary = *s;
                    return true;
                };

                size_t modelIndex = 0;
                std::optional<std::vector<size_t>> meshes;
                bool allMeshes = false;
                for (const std::string& arg : args) {
                    if (arg == "on" || arg == "off") {
                        settings.enabled = (arg == "on");
                        continue;
                    }
                    const size_t eq = arg.find('=');
                    if (eq == std::string::npos) {
                        return "ERR unknown argument '" + arg + "'";
                    }
                    const std::string key = arg.substr(0, eq);
                    const std::string value = arg.substr(eq + 1);
                    if (key == "up") {
                        float up[3] = {};
                        if (!ParseColor3(value, up) || up[0] * up[0] + up[1] * up[1] + up[2] * up[2] < 1e-6f) {
                            return "ERR up must be a nonzero x,y,z";
                        }
                        std::copy(up, up + 3, settings.up);
                    }
                    else if (key == "shift") {
                        if (!parsePair(value, settings.primaryShift, settings.secondaryShift)) {
                            return "ERR shift must be p,s";
                        }
                    }
                    else if (key == "width") {
                        float p = 0.0f, s = 0.0f;
                        if (!parsePair(value, p, s) || p < 0.0f || p > 1.0f || s < 0.0f || s > 1.0f) {
                            return "ERR width must be p,s in 0..1";
                        }
                        settings.primaryWidth = p;
                        settings.secondaryWidth = s;
                    }
                    else if (key == "strength") {
                        float p = 0.0f, s = 0.0f;
                        if (!parsePair(value, p, s) || p < 0.0f || s < 0.0f) {
                            return "ERR strength must be p,s >= 0";
                        }
                        settings.primaryStrength = p;
                        settings.secondaryStrength = s;
                    }
                    else if (key == "soft") {
                        const std::optional<float> soft = ParseFiniteFloat(value);
                        if (!soft || *soft < 0.0f || *soft > 0.5f) {
                            return "ERR soft must be 0..0.5";
                        }
                        settings.softness = *soft;
                    }
                    else if (key == "model") {
                        try { modelIndex = static_cast<size_t>(std::stoul(value)); }
                        catch (...) { return "ERR model must be an index"; }
                    }
                    else if (key == "mesh") {
                        if (value == "all") {
                            allMeshes = true;
                            continue;
                        }
                        std::vector<size_t> list;
                        if (!ParseMeshList(value, list)) {
                            return "ERR mesh must be all or a list like 0,7,8 or 18..64";
                        }
                        meshes = std::move(list);
                    }
                    else {
                        return "ERR unknown key '" + key + "'";
                    }
                }

                size_t flagged = 0;
                if (meshes || allMeshes) {
                    if (modelIndex >= models.size() || !models[modelIndex]) {
                        return "ERR model index out of range";
                    }
                    StaticModel* model = models[modelIndex];
                    for (size_t meshIndex = 0;; ++meshIndex) {
                        const Render::SurfaceMaterial* current = model->GetMaterial(meshIndex);
                        if (!current) {
                            break;
                        }
                        const bool hair = allMeshes ||
                            std::find(meshes->begin(), meshes->end(), meshIndex) != meshes->end();
                        Render::SurfaceMaterial material = *current;
                        material.toonHair = hair;
                        if (model->SetMaterial(meshIndex, material) && hair) {
                            ++flagged;
                        }
                    }
                    // The selector is baked into the proxies, so they must be rebuilt.
                    app.InvalidateRenderObjects();
                }

                renderer.SetToonHairSettings(settings);
                std::ostringstream out;
                out << std::fixed << std::setprecision(4);
                out << "OK enabled=" << (settings.enabled ? 1 : 0)
                    << " up=" << settings.up[0] << ',' << settings.up[1] << ',' << settings.up[2]
                    << " shift=" << settings.primaryShift << ',' << settings.secondaryShift
                    << " width=" << settings.primaryWidth << ',' << settings.secondaryWidth
                    << " strength=" << settings.primaryStrength << ',' << settings.secondaryStrength
                    << " soft=" << settings.softness;
                if (meshes || allMeshes) {
                    out << " hairMeshes=" << flagged;
                }
                return out.str();
            });
        registry.Register("toon.hairshadow", "[on|off | length=<m> | thickness=<m> | strength=<0..1>] - front-hair screen-space shadow cast by toon.hair meshes onto other toon surfaces",
            [&app](const std::vector<std::string>& args) -> std::string {
                Render::Renderer& renderer = app.GetRenderer();
                ToonHairShadowSettings settings = renderer.GetToonHairShadowSettings();
                for (const std::string& arg : args) {
                    if (arg == "on" || arg == "off") {
                        settings.enabled = (arg == "on");
                        continue;
                    }
                    const size_t eq = arg.find('=');
                    if (eq == std::string::npos) {
                        return "ERR unknown argument '" + arg + "'";
                    }
                    const std::string key = arg.substr(0, eq);
                    const std::optional<float> value = ParseFiniteFloat(arg.substr(eq + 1));
                    if (key == "length") {
                        if (!value || *value <= 0.0f || *value > 1.0f) {
                            return "ERR length must be in (0, 1] metres";
                        }
                        settings.length = *value;
                    }
                    else if (key == "thickness") {
                        if (!value || *value <= 0.0f || *value > 1.0f) {
                            return "ERR thickness must be in (0, 1] metres";
                        }
                        settings.thickness = *value;
                    }
                    else if (key == "strength") {
                        if (!value || *value < 0.0f || *value > 1.0f) {
                            return "ERR strength must be 0..1";
                        }
                        settings.strength = *value;
                    }
                    else {
                        return "ERR unknown key '" + key + "'";
                    }
                }

                renderer.SetToonHairShadowSettings(settings);
                std::ostringstream out;
                out << std::fixed << std::setprecision(4);
                out << "OK enabled=" << (settings.enabled ? 1 : 0)
                    << " length=" << settings.length
                    << " thickness=" << settings.thickness
                    << " strength=" << settings.strength;
                return out.str();
            });
        registry.Register("toon.headnormal", "[on|off | center=x,y,z | radius=<m> | blend=<0..1>] - pull non-hair toon normals inside a head sphere toward the sphere normal",
            [&app](const std::vector<std::string>& args) -> std::string {
                Render::Renderer& renderer = app.GetRenderer();
                ToonHeadNormalSettings settings = renderer.GetToonHeadNormalSettings();
                for (const std::string& arg : args) {
                    if (arg == "on" || arg == "off") {
                        settings.enabled = (arg == "on");
                        continue;
                    }
                    const size_t eq = arg.find('=');
                    if (eq == std::string::npos) {
                        return "ERR unknown argument '" + arg + "'";
                    }
                    const std::string key = arg.substr(0, eq);
                    const std::string value = arg.substr(eq + 1);
                    if (key == "center") {
                        float center[3] = {};
                        if (!ParseColor3(value, center)) {
                            return "ERR center must be x,y,z";
                        }
                        std::copy(center, center + 3, settings.center);
                    }
                    else if (key == "radius") {
                        const std::optional<float> radius = ParseFiniteFloat(value);
                        if (!radius || *radius <= 0.0f || *radius > 10.0f) {
                            return "ERR radius must be in (0, 10] metres";
                        }
                        settings.radius = *radius;
                    }
                    else if (key == "blend") {
                        const std::optional<float> blend = ParseFiniteFloat(value);
                        if (!blend || *blend < 0.0f || *blend > 1.0f) {
                            return "ERR blend must be 0..1";
                        }
                        settings.blend = *blend;
                    }
                    else {
                        return "ERR unknown key '" + key + "'";
                    }
                }

                renderer.SetToonHeadNormalSettings(settings);
                std::ostringstream out;
                out << std::fixed << std::setprecision(4);
                out << "OK enabled=" << (settings.enabled ? 1 : 0)
                    << " center=" << settings.center[0] << ',' << settings.center[1] << ',' << settings.center[2]
                    << " radius=" << settings.radius
                    << " blend=" << settings.blend;
                return out.str();
            });
    }
}
