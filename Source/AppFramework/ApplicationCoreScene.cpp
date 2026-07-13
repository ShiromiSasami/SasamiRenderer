#include "ApplicationCore.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "Object/Camera.h"
#include "Object/PointLight.h"
#include "Object/SpotLight.h"
#include "Object/StaticModel.h"
#include "AppFramework/Light/DirectionalLight.h"

namespace SasamiRenderer
{
    namespace
    {
        void WriteFloat3(std::ofstream& out, const char* xKey, const char* yKey, const char* zKey,
                         float x, float y, float z)
        {
            out << xKey << " = " << x << '\n'
                << yKey << " = " << y << '\n'
                << zKey << " = " << z << '\n';
        }

        float ParseFloat(const std::unordered_map<std::string, std::string>& kv,
                         const char* key, float fallback = 0.0f)
        {
            const auto it = kv.find(key);
            if (it == kv.end()) return fallback;
            try { return std::stof(it->second); } catch (...) { return fallback; }
        }

        long ParseInt(const std::unordered_map<std::string, std::string>& kv,
                      const char* key, long fallback = 0)
        {
            const auto it = kv.find(key);
            if (it == kv.end()) return fallback;
            try { return std::stol(it->second); } catch (...) { return fallback; }
        }

        std::string ParseString(const std::unordered_map<std::string, std::string>& kv,
                                const char* key, const char* fallback = "")
        {
            const auto it = kv.find(key);
            return (it != kv.end()) ? it->second : fallback;
        }

        static const char* kHeader = "# SasamiRenderer Scene v1\n";

        // Parses an INI-like stream. Invokes onSection(sectionName, kv) at every
        // section boundary and once at EOF for the final section.
        template <typename OnSection>
        void ParseIniStream(std::istream& in, OnSection&& onSection)
        {
            std::string currentSection;
            std::unordered_map<std::string, std::string> kv;

            const auto flush = [&]() {
                onSection(currentSection, kv);
                kv.clear();
            };

            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;

                if (line[0] == '[') {
                    flush();
                    const auto close = line.find(']');
                    currentSection = (close != std::string::npos)
                        ? line.substr(1, close - 1) : "";
                    continue;
                }

                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;

                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);

                const auto ltrim = [](std::string& s) {
                    const auto it = s.find_first_not_of(" \t\r\n");
                    if (it != std::string::npos) s = s.substr(it);
                    else s.clear();
                };
                const auto rtrim = [](std::string& s) {
                    const auto it = s.find_last_not_of(" \t\r\n");
                    if (it != std::string::npos) s = s.substr(0, it + 1);
                    else s.clear();
                };
                ltrim(key); rtrim(key);
                ltrim(val); rtrim(val);

                if (!key.empty()) kv[key] = val;
            }
            flush(); // handle last section
        }
    }

    bool ApplicationCore::SaveScene(const std::string& path) const
    {
        std::ofstream out(path);
        if (!out.is_open()) return false;

        out << kHeader << '\n';

        // --- directional_light ---
        const DirectionalLight dl = GetDirectionalLight();
        out << "[directional_light]\n";
        out << "yaw = "        << dl.yaw        << '\n';
        out << "pitch = "      << dl.pitch       << '\n';
        out << "distance = "   << dl.distance    << '\n';
        out << "ortho_half = " << dl.orthoHalf   << '\n';
        out << "near_z = "     << dl.nearZ       << '\n';
        out << "far_z = "      << dl.farZ        << '\n';
        out << "intensity = "  << dl.intensity   << '\n';
        out << "r = "          << dl.color.r     << '\n';
        out << "g = "          << dl.color.g     << '\n';
        out << "b = "          << dl.color.b     << '\n';
        out << "shadow_mode = " << static_cast<int>(dl.shadowMode) << '\n';
        out << "shadow_distance = " << dl.shadowDistance << '\n';
        out << "cascade_exponent = " << dl.cascadeDistributionExponent << '\n';
        out << "cascade_blend = " << dl.cascadeBlendFraction << '\n';
        out << "depth_range_mode = " << static_cast<int>(dl.depthRangeMode) << '\n';
        out << "depth_bias = " << dl.depthBias << '\n';
        out << "slope_scale_bias = " << dl.slopeScaleBias << '\n';
        out << "normal_bias = " << dl.normalBias << '\n';
        out << "far_bias_scale = " << dl.farBiasScale << '\n';
        out << '\n';

        // --- camera ---
        const Camera* cam = GetActiveCamera();
        if (cam) {
            out << "[camera]\n";
            WriteFloat3(out, "position_x", "position_y", "position_z",
                        cam->Transform().position.x,
                        cam->Transform().position.y,
                        cam->Transform().position.z);
            WriteFloat3(out, "target_x", "target_y", "target_z",
                        cam->Transform().position.x,
                        cam->Transform().position.y,
                        cam->Transform().position.z);
            out << "yaw = "        << cam->Yaw()       << '\n';
            out << "pitch = "      << cam->Pitch()     << '\n';
            out << "distance = "   << cam->Distance()  << '\n';
            out << "near_clip = "  << cam->NearClip()  << '\n';
            out << "far_clip = "   << cam->FarClip()   << '\n';
            out << "move_speed = " << cam->MoveSpeed() << '\n';
            out << '\n';
        }

        // --- static_model ---
        for (const StaticModel* model : GetStaticModels()) {
            if (model->GetModelPath().empty()) continue;
            const Vector3 t = model->GetModelTranslation();
            const char* fmt = (model->GetModelFormat() == StaticModel::ModelFormat::Obj) ? "obj" : "gltf";
            out << "[static_model]\n";
            out << "path = "          << model->GetModelPath()         << '\n';
            out << "format = "        << fmt                            << '\n';
            out << "uniform_scale = " << model->GetModelUniformScale() << '\n';
            out << "x = "             << t.x                           << '\n';
            out << "y = "             << t.y                           << '\n';
            out << "z = "             << t.z                           << '\n';
            out << '\n';
        }

        // --- point_light ---
        for (const PointLight* pl : GetPointLightObjects()) {
            out << "[point_light]\n";
            out << "x = "         << pl->Transform().position.x << '\n';
            out << "y = "         << pl->Transform().position.y << '\n';
            out << "z = "         << pl->Transform().position.z << '\n';
            out << "range = "     << pl->Range()                << '\n';
            out << "intensity = " << pl->Intensity()            << '\n';
            out << "r = "         << pl->ColorData()[0]         << '\n';
            out << "g = "         << pl->ColorData()[1]         << '\n';
            out << "b = "         << pl->ColorData()[2]         << '\n';
            out << '\n';
        }

        // --- spot_light ---
        for (const SpotLight* sl : GetSpotLightObjects()) {
            out << "[spot_light]\n";
            out << "x = "           << sl->Transform().position.x  << '\n';
            out << "y = "           << sl->Transform().position.y  << '\n';
            out << "z = "           << sl->Transform().position.z  << '\n';
            out << "pitch = "       << sl->Transform().rotation.pitch << '\n';
            out << "yaw = "         << sl->Transform().rotation.yaw   << '\n';
            out << "range = "       << sl->Range()                  << '\n';
            out << "intensity = "   << sl->Intensity()              << '\n';
            out << "inner_angle = " << sl->InnerAngle()             << '\n';
            out << "outer_angle = " << sl->OuterAngle()             << '\n';
            out << "r = "           << sl->ColorData()[0]           << '\n';
            out << "g = "           << sl->ColorData()[1]           << '\n';
            out << "b = "           << sl->ColorData()[2]           << '\n';
            out << '\n';
        }

        return true;
    }

    bool ApplicationCore::LoadScene(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open()) return false;

        ClearObjects();

        ParseIniStream(in, [&](const std::string& currentSection,
                               const std::unordered_map<std::string, std::string>& kv) {
            if (currentSection == "directional_light") {
                DirectionalLight dl = GetDirectionalLight();
                dl.yaw       = ParseFloat(kv, "yaw",       dl.yaw);
                dl.pitch     = ParseFloat(kv, "pitch",     dl.pitch);
                dl.distance  = ParseFloat(kv, "distance",  dl.distance);
                dl.orthoHalf = ParseFloat(kv, "ortho_half",dl.orthoHalf);
                dl.nearZ     = ParseFloat(kv, "near_z",    dl.nearZ);
                dl.farZ      = ParseFloat(kv, "far_z",     dl.farZ);
                dl.intensity = ParseFloat(kv, "intensity", dl.intensity);
                dl.color.r   = ParseFloat(kv, "r",         dl.color.r);
                dl.color.g   = ParseFloat(kv, "g",         dl.color.g);
                dl.color.b   = ParseFloat(kv, "b",         dl.color.b);
                dl.shadowMode = static_cast<DirectionalShadowMode>(
                    std::clamp(ParseInt(kv, "shadow_mode", static_cast<long>(dl.shadowMode)), 0L, 3L));
                dl.shadowDistance = ParseFloat(kv, "shadow_distance", dl.shadowDistance);
                dl.cascadeDistributionExponent = ParseFloat(kv, "cascade_exponent", dl.cascadeDistributionExponent);
                dl.cascadeBlendFraction = ParseFloat(kv, "cascade_blend", dl.cascadeBlendFraction);
                dl.depthRangeMode = static_cast<DirectionalShadowDepthRangeMode>(
                    std::clamp(ParseInt(kv, "depth_range_mode", static_cast<long>(dl.depthRangeMode)), 0L, 1L));
                dl.depthBias = ParseFloat(kv, "depth_bias", dl.depthBias);
                dl.slopeScaleBias = ParseFloat(kv, "slope_scale_bias", dl.slopeScaleBias);
                dl.normalBias = ParseFloat(kv, "normal_bias", dl.normalBias);
                dl.farBiasScale = ParseFloat(kv, "far_bias_scale", dl.farBiasScale);
                SetDirectionalLight(dl);
            }
            else if (currentSection == "camera") {
                Camera* cam = CreateCameraObject();
                cam->SetTarget(
                    ParseFloat(kv, "position_x", ParseFloat(kv, "target_x")),
                    ParseFloat(kv, "position_y", ParseFloat(kv, "target_y")),
                    ParseFloat(kv, "position_z", ParseFloat(kv, "target_z")));
                cam->SetYawPitch(
                    ParseFloat(kv, "yaw"),
                    ParseFloat(kv, "pitch"));
                cam->SetDistance(ParseFloat(kv, "distance", 2.5f));
                cam->SetClipPlanes(
                    ParseFloat(kv, "near_clip", 0.0005f),
                    ParseFloat(kv, "far_clip",  500.0f));
                cam->SetMoveSpeed(ParseFloat(kv, "move_speed", 1.0f));
                SetMainCamera(cam);
            }
            else if (currentSection == "static_model") {
                const std::string meshPath = ParseString(kv, "path");
                if (!meshPath.empty()) {
                    const std::string fmt = ParseString(kv, "format", "gltf");
                    const float scale = ParseFloat(kv, "uniform_scale", 1.0f);
                    StaticModel::ModelFormat format = (fmt == "obj")
                        ? StaticModel::ModelFormat::Obj
                        : StaticModel::ModelFormat::Gltf;
                    StaticModel* model = CreateStaticModel();
                    if (model->LoadModel(meshPath, format, scale)) {
                        model->SetTranslation(
                            ParseFloat(kv, "x"),
                            ParseFloat(kv, "y"),
                            ParseFloat(kv, "z"));
                    } else {
                        DeleteObject(model);
                    }
                }
            }
            else if (currentSection == "point_light") {
                PointLight* pl = CreatePointLightObject();
                pl->Transform().position.x = ParseFloat(kv, "x");
                pl->Transform().position.y = ParseFloat(kv, "y");
                pl->Transform().position.z = ParseFloat(kv, "z");
                pl->SetRange(ParseFloat(kv, "range", 5.0f));
                pl->SetIntensity(ParseFloat(kv, "intensity", 1.0f));
                pl->ColorData()[0] = ParseFloat(kv, "r", 1.0f);
                pl->ColorData()[1] = ParseFloat(kv, "g", 1.0f);
                pl->ColorData()[2] = ParseFloat(kv, "b", 1.0f);
            }
            else if (currentSection == "spot_light") {
                SpotLight* sl = CreateSpotLightObject();
                sl->Transform().position.x      = ParseFloat(kv, "x");
                sl->Transform().position.y      = ParseFloat(kv, "y");
                sl->Transform().position.z      = ParseFloat(kv, "z");
                sl->Transform().rotation.pitch  = ParseFloat(kv, "pitch");
                sl->Transform().rotation.yaw    = ParseFloat(kv, "yaw");
                sl->SetRange(ParseFloat(kv, "range", 6.0f));
                sl->SetIntensity(ParseFloat(kv, "intensity", 1.0f));
                sl->SetInnerAngle(ParseFloat(kv, "inner_angle", 0.261799f));
                sl->SetOuterAngle(ParseFloat(kv, "outer_angle", 0.436332f));
                sl->ColorData()[0] = ParseFloat(kv, "r", 1.0f);
                sl->ColorData()[1] = ParseFloat(kv, "g", 1.0f);
                sl->ColorData()[2] = ParseFloat(kv, "b", 1.0f);
            }
        });

        InvalidateRenderObjects();
        return true;
    }

    bool ApplicationCore::ApplyCameraAndLights(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open()) return false;

        // Non-destructive: apply onto the currently active camera and existing
        // directional light; point/spot lights from the file are appended. The
        // procedural scene geometry created by the app is left untouched.
        ParseIniStream(in, [&](const std::string& currentSection,
                               const std::unordered_map<std::string, std::string>& kv) {
            if (currentSection == "directional_light") {
                DirectionalLight dl = GetDirectionalLight();
                dl.yaw       = ParseFloat(kv, "yaw",       dl.yaw);
                dl.pitch     = ParseFloat(kv, "pitch",     dl.pitch);
                dl.distance  = ParseFloat(kv, "distance",  dl.distance);
                dl.orthoHalf = ParseFloat(kv, "ortho_half",dl.orthoHalf);
                dl.nearZ     = ParseFloat(kv, "near_z",    dl.nearZ);
                dl.farZ      = ParseFloat(kv, "far_z",     dl.farZ);
                dl.intensity = ParseFloat(kv, "intensity", dl.intensity);
                dl.color.r   = ParseFloat(kv, "r",         dl.color.r);
                dl.color.g   = ParseFloat(kv, "g",         dl.color.g);
                dl.color.b   = ParseFloat(kv, "b",         dl.color.b);
                dl.shadowMode = static_cast<DirectionalShadowMode>(
                    std::clamp(ParseInt(kv, "shadow_mode", static_cast<long>(dl.shadowMode)), 0L, 3L));
                dl.shadowDistance = ParseFloat(kv, "shadow_distance", dl.shadowDistance);
                dl.cascadeDistributionExponent = ParseFloat(kv, "cascade_exponent", dl.cascadeDistributionExponent);
                dl.cascadeBlendFraction = ParseFloat(kv, "cascade_blend", dl.cascadeBlendFraction);
                dl.depthRangeMode = static_cast<DirectionalShadowDepthRangeMode>(
                    std::clamp(ParseInt(kv, "depth_range_mode", static_cast<long>(dl.depthRangeMode)), 0L, 1L));
                dl.depthBias = ParseFloat(kv, "depth_bias", dl.depthBias);
                dl.slopeScaleBias = ParseFloat(kv, "slope_scale_bias", dl.slopeScaleBias);
                dl.normalBias = ParseFloat(kv, "normal_bias", dl.normalBias);
                dl.farBiasScale = ParseFloat(kv, "far_bias_scale", dl.farBiasScale);
                SetDirectionalLight(dl);
            }
            else if (currentSection == "camera") {
                Camera* cam = GetActiveCamera();
                if (!cam) cam = CreateCameraObject();
                if (!cam) return;
                cam->SetTarget(
                    ParseFloat(kv, "position_x", ParseFloat(kv, "target_x")),
                    ParseFloat(kv, "position_y", ParseFloat(kv, "target_y")),
                    ParseFloat(kv, "position_z", ParseFloat(kv, "target_z")));
                cam->SetYawPitch(
                    ParseFloat(kv, "yaw"),
                    ParseFloat(kv, "pitch"));
                cam->SetDistance(ParseFloat(kv, "distance", 2.5f));
                cam->SetClipPlanes(
                    ParseFloat(kv, "near_clip", 0.0005f),
                    ParseFloat(kv, "far_clip",  500.0f));
                cam->SetMoveSpeed(ParseFloat(kv, "move_speed", 1.0f));
                SetMainCamera(cam);
            }
            else if (currentSection == "point_light") {
                PointLight* pl = CreatePointLightObject();
                pl->Transform().position.x = ParseFloat(kv, "x");
                pl->Transform().position.y = ParseFloat(kv, "y");
                pl->Transform().position.z = ParseFloat(kv, "z");
                pl->SetRange(ParseFloat(kv, "range", 5.0f));
                pl->SetIntensity(ParseFloat(kv, "intensity", 1.0f));
                pl->ColorData()[0] = ParseFloat(kv, "r", 1.0f);
                pl->ColorData()[1] = ParseFloat(kv, "g", 1.0f);
                pl->ColorData()[2] = ParseFloat(kv, "b", 1.0f);
            }
            else if (currentSection == "spot_light") {
                SpotLight* sl = CreateSpotLightObject();
                sl->Transform().position.x      = ParseFloat(kv, "x");
                sl->Transform().position.y      = ParseFloat(kv, "y");
                sl->Transform().position.z      = ParseFloat(kv, "z");
                sl->Transform().rotation.pitch  = ParseFloat(kv, "pitch");
                sl->Transform().rotation.yaw    = ParseFloat(kv, "yaw");
                sl->SetRange(ParseFloat(kv, "range", 6.0f));
                sl->SetIntensity(ParseFloat(kv, "intensity", 1.0f));
                sl->SetInnerAngle(ParseFloat(kv, "inner_angle", 0.261799f));
                sl->SetOuterAngle(ParseFloat(kv, "outer_angle", 0.436332f));
                sl->ColorData()[0] = ParseFloat(kv, "r", 1.0f);
                sl->ColorData()[1] = ParseFloat(kv, "g", 1.0f);
                sl->ColorData()[2] = ParseFloat(kv, "b", 1.0f);
            }
            // [static_model] and unknown sections are intentionally ignored.
        });

        InvalidateRenderObjects();
        return true;
    }
} // namespace SasamiRenderer
