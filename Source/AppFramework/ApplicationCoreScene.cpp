#include "ApplicationCore.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "Object/Camera.h"
#include "Object/PointLight.h"
#include "Object/SpotLight.h"
#include "Object/StaticModel.h"
#include "AppFramework/Light/DirectionalLight.h"
#include "AppFramework/Config/IniStreamParser.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiEngine::App
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

        // Shared by LoadScene and ApplyCameraAndLights so the [camera] section is only
        // decoded in one place. Fallbacks here must be kept in sync with Camera's own
        // member-initializer defaults (Camera.h); a mismatch means a scene file that
        // omits a key silently ends up with different clip planes than a brand-new
        // Camera, which is exactly the near_clip = 0.0005f bug this was pulled out to fix.
        void ApplyCameraSection(const std::unordered_map<std::string, std::string>& kv, Camera& camera)
        {
            camera.SetTarget(
                ParseFloat(kv, "position_x", ParseFloat(kv, "target_x")),
                ParseFloat(kv, "position_y", ParseFloat(kv, "target_y")),
                ParseFloat(kv, "position_z", ParseFloat(kv, "target_z")));
            camera.SetYawPitch(
                ParseFloat(kv, "yaw"),
                ParseFloat(kv, "pitch"));
            camera.SetDistance(ParseFloat(kv, "distance", 2.5f));
            camera.SetClipPlanes(
                ParseFloat(kv, "near_clip", 0.05f),
                ParseFloat(kv, "far_clip",  500.0f));
            camera.SetMoveSpeed(ParseFloat(kv, "move_speed", 1.0f));
        }

        // One placement of an [instance] section, "tN = ..." with whitespace-separated floats:
        //   3 values  -> translation
        //   5 values  -> translation, yaw (radians, about +Y), uniform scale
        //   16 values -> full row-major matrix (row-vector convention, translation in the last row)
        // Anything else is rejected so a truncated line cannot silently place an instance at the origin.
        bool ParseInstanceTransform(const std::string& text, StaticModel::InstanceTransform& out)
        {
            std::istringstream stream(text);
            float values[16] = {};
            size_t count = 0;
            float value = 0.0f;
            while (count < 16 && stream >> value) {
                values[count++] = value;
            }
            std::string trailing;
            if (stream >> trailing) {
                return false;
            }

            if (count == 16) {
                std::copy(values, values + 16, out.begin());
                return true;
            }
            if (count != 3 && count != 5) {
                return false;
            }

            const float yaw   = (count == 5) ? values[3] : 0.0f;
            const float scale = (count == 5) ? values[4] : 1.0f;
            const float c = std::cos(yaw) * scale;
            const float s = std::sin(yaw) * scale;
            out = {
                c,    0.0f,  -s,   0.0f,
                0.0f, scale, 0.0f, 0.0f,
                s,    0.0f,  c,    0.0f,
                values[0], values[1], values[2], 1.0f,
            };
            return true;
        }

        StaticModel::ModelFormat ParseModelFormat(const std::string& fmt)
        {
            if (fmt == "obj") return StaticModel::ModelFormat::Obj;
            if (fmt == "fbx") return StaticModel::ModelFormat::Fbx;
            return StaticModel::ModelFormat::Gltf;
        }

        const char* ModelFormatName(StaticModel::ModelFormat format)
        {
            switch (format) {
            case StaticModel::ModelFormat::Obj: return "obj";
            case StaticModel::ModelFormat::Fbx: return "fbx";
            default:                            return "gltf";
            }
        }

        static const char* kHeader = "# SasamiRenderer Scene v1\n";
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
        out << "contact_shadow = " << (dl.contactShadowEnabled ? 1 : 0) << '\n';
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
            if (!model->GetInstanceTransforms().empty()) continue; // written as [instance] below
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

        // --- instance ---
        // Always written as full matrices so a save/load round trip is lossless whatever form
        // (3/5/16 values) the placements were authored in.
        for (const StaticModel* model : GetStaticModels()) {
            const auto& instances = model->GetInstanceTransforms();
            if (model->GetModelPath().empty() || instances.empty()) continue;
            const Vector3 t = model->GetModelTranslation();
            out << "[instance]\n";
            out << "path = "          << model->GetModelPath()                    << '\n';
            out << "format = "        << ModelFormatName(model->GetModelFormat()) << '\n';
            out << "uniform_scale = " << model->GetModelUniformScale()            << '\n';
            out << "x = "             << t.x                                      << '\n';
            out << "y = "             << t.y                                      << '\n';
            out << "z = "             << t.z                                      << '\n';
            out << "count = "         << instances.size()                         << '\n';
            for (size_t i = 0; i < instances.size(); ++i) {
                out << 't' << i << " =";
                for (const float v : instances[i]) out << ' ' << v;
                out << '\n';
            }
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
                dl.contactShadowEnabled =
                    ParseFloat(kv, "contact_shadow", dl.contactShadowEnabled ? 1.0f : 0.0f) != 0.0f;
                SetDirectionalLight(dl);
            }
            else if (currentSection == "camera") {
                Camera* cam = CreateCameraObject();
                if (!cam) return;
                ApplyCameraSection(kv, *cam);
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
                    if (!model) return;
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
            else if (currentSection == "instance") {
                // One model loaded once and placed `count` times. Every placement's proxies share
                // the loaded Mesh pointers, so SceneSubmitter resolves them to one mesh entry each.
                const std::string meshPath = ParseString(kv, "path");
                const long count = ParseInt(kv, "count");
                if (meshPath.empty() || count <= 0) return;

                std::vector<StaticModel::InstanceTransform> transforms;
                transforms.reserve(static_cast<size_t>(count));
                for (long i = 0; i < count; ++i) {
                    const std::string key = "t" + std::to_string(i);
                    StaticModel::InstanceTransform transform{};
                    if (!ParseInstanceTransform(ParseString(kv, key.c_str()), transform)) {
                        const std::string message = "[Scene] [instance] " + meshPath + ": skipped malformed or missing " + key + "\n";
                        DebugLog(message.c_str());
                        continue;
                    }
                    transforms.push_back(transform);
                }
                if (transforms.empty()) return;

                StaticModel* model = CreateStaticModel();
                if (!model) return;
                if (!model->LoadModel(meshPath, ParseModelFormat(ParseString(kv, "format", "gltf")),
                                      ParseFloat(kv, "uniform_scale", 1.0f))) {
                    DeleteObject(model);
                    return;
                }
                model->SetTranslation(ParseFloat(kv, "x"), ParseFloat(kv, "y"), ParseFloat(kv, "z"));
                model->SetInstanceTransforms(std::move(transforms));
            }
            else if (currentSection == "point_light") {
                PointLight* pl = CreatePointLightObject();
                if (!pl) return;
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
                if (!sl) return;
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

        // ClearObjects() destroyed the old camera. A file without a [camera] section used to leave
        // the app with none, so no frame rendered and ImGui's frame was never closed (its font
        // stack then leaked once per frame until the error tooltip recursed into a stack overflow).
        if (GetMainCamera() == nullptr) {
            if (Camera* cam = CreateCameraObject()) {
                ApplyCameraSection({}, *cam);
                SetMainCamera(cam);
                DebugLog("[Scene] no [camera] section; created a default camera\n");
            }
        }

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
                dl.contactShadowEnabled =
                    ParseFloat(kv, "contact_shadow", dl.contactShadowEnabled ? 1.0f : 0.0f) != 0.0f;
                SetDirectionalLight(dl);
            }
            else if (currentSection == "camera") {
                Camera* cam = GetActiveCamera();
                if (!cam) cam = CreateCameraObject();
                if (!cam) return;
                ApplyCameraSection(kv, *cam);
                SetMainCamera(cam);
            }
            else if (currentSection == "point_light") {
                PointLight* pl = CreatePointLightObject();
                if (!pl) return;
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
                if (!sl) return;
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
            // [static_model], [instance] and unknown sections are intentionally ignored.
        });

        InvalidateRenderObjects();
        return true;
    }
} // namespace SasamiEngine::App
