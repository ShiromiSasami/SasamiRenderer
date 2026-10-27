#include "AppFramework/Debug/RemoteControl/DebugSceneCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/ApplicationResourcePaths.h"
#include "AppFramework/Loader/AssetLoader.h"
#include "AppFramework/Object/StaticModel.h"
#include "AppFramework/Placement/InstanceScatter.h"
#include "AppFramework/Placement/SplinePlacement.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/SpotLight.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        std::optional<StaticModel::ModelFormat> ParseModelFormatArg(const std::string& text)
        {
            if (text == "obj") {
                return StaticModel::ModelFormat::Obj;
            }
            if (text == "fbx") {
                return StaticModel::ModelFormat::Fbx;
            }
            if (text == "gltf") {
                return StaticModel::ModelFormat::Gltf;
            }
            return std::nullopt;
        }

        // The tail shared by the placement commands: one model object at the origin carrying every
        // generated placement, which scene.save then writes out as an [instance] section.
        std::string SpawnInstancedModel(ApplicationCore& app, const std::string& path, StaticModel::ModelFormat format,
                                        float uniformScale, std::vector<StaticModel::InstanceTransform> transforms)
        {
            StaticModel* model = app.CreateStaticModel();
            if (!model) {
                return "ERR could not create model object";
            }
            if (!model->LoadModel(path, format, uniformScale)) {
                app.DeleteObject(model);
                return "ERR could not load model " + path;
            }
            model->SetTranslation(0.0f, 0.0f, 0.0f);
            model->SetInstanceTransforms(std::move(transforms));
            return {};
        }

    }

    void RegisterSceneCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("ping", "connectivity check", [](const std::vector<std::string>&) {
            return std::string("pong");
        });

        registry.Register("camera.get", "current fly-camera position, pan scale, and orientation (yaw/pitch in radians)",
            [&app](const std::vector<std::string>&) -> std::string {
                const Camera* camera = app.GetMainCamera();
                if (camera == nullptr) {
                    return "ERR no active camera";
                }
                const auto& transform = camera->Transform();
                return "OK target=" + FormatFloat(transform.position.x)
                     + "," + FormatFloat(transform.position.y)
                     + "," + FormatFloat(transform.position.z)
                     + " yaw=" + FormatFloat(transform.rotation.yaw)
                     + " pitch=" + FormatFloat(transform.rotation.pitch)
                     + " panScale=" + FormatFloat(camera->Distance());
            });

        registry.Register("camera.setTarget", "<x> <y> <z> - set fly-camera position (legacy command name)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() != 3) {
                    return "ERR usage: camera.setTarget <x> <y> <z>";
                }
                Camera* camera = app.GetMainCamera();
                if (camera == nullptr) {
                    return "ERR no active camera";
                }
                const auto x = ParseFloat(args[0]);
                const auto y = ParseFloat(args[1]);
                const auto z = ParseFloat(args[2]);
                if (!x || !y || !z) {
                    return "ERR arguments must be numbers";
                }
                camera->SetTarget(*x, *y, *z);
                return "OK";
            });

        registry.Register("camera.setYawPitch", "<yaw-rad> <pitch-rad> - set camera orientation (radians, not degrees)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() != 2) {
                    return "ERR usage: camera.setYawPitch <yaw-rad> <pitch-rad>";
                }
                Camera* camera = app.GetMainCamera();
                if (camera == nullptr) {
                    return "ERR no active camera";
                }
                const auto yaw = ParseFloat(args[0]);
                const auto pitch = ParseFloat(args[1]);
                if (!yaw || !pitch) {
                    return "ERR arguments must be numbers";
                }
                camera->SetYawPitch(*yaw, *pitch);
                return "OK";
            });

        // Camera is a fly camera: Transform().position is the eye position and there is no
        // target-minus-forward orbit calculation. The legacy `distance` field only scales
        // shift-drag panning, so expose that actual behavior instead of claiming this moves
        // the camera. Keep the command name for script compatibility.
        registry.Register("camera.setDistance", "<d> - set shift-drag pan-speed scale (legacy name; not orbit distance)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() != 1) {
                    return "ERR usage: camera.setDistance <d>";
                }
                Camera* camera = app.GetMainCamera();
                if (camera == nullptr) {
                    return "ERR no active camera";
                }
                const auto distance = ParseFloat(args[0]);
                if (!distance) {
                    return "ERR argument must be a number";
                }
                camera->SetDistance(*distance);
                return "OK panScale=" + FormatFloat(camera->Distance());
            });

        registry.Register("light.spot.list", "positions and cone angles of every spot light",
            [&app](const std::vector<std::string>&) -> std::string {
                const std::vector<SpotLight*> lights = app.GetSpotLightObjects();
                if (lights.empty()) {
                    return "OK count=0";
                }
                std::string out = "OK count=" + std::to_string(lights.size());
                for (size_t i = 0; i < lights.size(); ++i) {
                    const SpotLight* light = lights[i];
                    if (light == nullptr) {
                        continue;
                    }
                    const auto& transform = light->Transform();
                    out += " [" + std::to_string(i)
                         + " pos=" + FormatFloat(transform.position.x)
                         + "," + FormatFloat(transform.position.y)
                         + "," + FormatFloat(transform.position.z)
                         + " yaw=" + FormatFloat(transform.rotation.yaw)
                         + " pitch=" + FormatFloat(transform.rotation.pitch)
                         + " range=" + FormatFloat(light->Range())
                         + " inner=" + FormatFloat(light->InnerAngle())
                         + " outer=" + FormatFloat(light->OuterAngle())
                         + " intensity=" + FormatFloat(light->Intensity())
                         + "]";
                }
                return out;
            });

        registry.Register("scene.save", "<path> - write the scene to disk",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "ERR usage: scene.save <path>";
                }
                return app.SaveScene(JoinFrom(args, 0)) ? "OK" : "ERR save failed";
            });

        registry.Register("scene.load", "<path> - load a scene from disk",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "ERR usage: scene.load <path>";
                }
                return app.LoadScene(JoinFrom(args, 0)) ? "OK" : "ERR load failed";
            });

        registry.Register("scene.models", "list static model indices with their translations (first 64)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: scene.models";
                }
                const std::vector<StaticModel*> models = app.GetStaticModels();
                std::string out = "OK count=" + std::to_string(models.size());
                constexpr size_t MAX_LISTED = 64;
                for (size_t i = 0; i < models.size() && i < MAX_LISTED; ++i) {
                    if (models[i] == nullptr) {
                        continue;
                    }
                    const Vector3 pos = models[i]->GetModelTranslation();
                    out += " " + std::to_string(i) + ":" + FormatFloat(pos.x) + "," + FormatFloat(pos.y) + "," + FormatFloat(pos.z);
                }
                if (models.size() > MAX_LISTED) {
                    out += " ...";
                }
                return out;
            });

        registry.Register("scene.move",
            "<staticModelIndex> <dx> <dy> <dz> - translate a static model by a delta (index as in scene.models); the render proxy rebuilds on the"
            " next frame, so issue voxel.update in a later command",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() != 4) {
                    return "ERR usage: scene.move <staticModelIndex> <dx> <dy> <dz>";
                }
                const auto index = ParseFloat(args[0]);
                const auto dx = ParseFloat(args[1]);
                const auto dy = ParseFloat(args[2]);
                const auto dz = ParseFloat(args[3]);
                if (!index || !dx || !dy || !dz) {
                    return "ERR arguments must be numbers";
                }
                const std::vector<StaticModel*> models = app.GetStaticModels();
                if (*index < 0.0f || *index >= static_cast<float>(models.size()) || models[static_cast<size_t>(*index)] == nullptr) {
                    return "ERR model index out of range (count=" + std::to_string(models.size()) + ")";
                }
                StaticModel* model = models[static_cast<size_t>(*index)];
                const Vector3 pos = model->GetModelTranslation();
                const float x = pos.x + *dx;
                const float y = pos.y + *dy;
                const float z = pos.z + *dz;
                model->SetTranslation(x, y, z);
                app.InvalidateRenderObjects();
                return "OK model=" + std::to_string(static_cast<size_t>(*index))
                     + " pos=" + FormatFloat(x) + "," + FormatFloat(y) + "," + FormatFloat(z);
            });

        registry.Register("scene.scatter",
            "<model> <obj|gltf|fbx> <density-image> <count> <minX> <minZ> <maxX> <maxZ> [y] [seed] [scaleMin] [scaleMax] [uniformScale]"
            " - place <count> instances of a model by density-map rejection sampling (R channel; image top = minZ); scene.save bakes them",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.size() < 8) {
                    return "ERR usage: scene.scatter <model> <obj|gltf|fbx> <density-image> <count> <minX> <minZ> <maxX> <maxZ>"
                           " [y] [seed] [scaleMin] [scaleMax] [uniformScale]";
                }

                std::optional<float> numbers[10];
                for (size_t i = 3; i < args.size() && i < 13; ++i) {
                    numbers[i - 3] = ParseFloat(args[i]);
                    if (!numbers[i - 3]) {
                        return "ERR not a number: " + args[i];
                    }
                }
                const auto optional = [&](size_t i, float fallback) { return numbers[i] ? *numbers[i] : fallback; };

                const float count = *numbers[0];
                if (count < 1.0f || count > 1.0e7f) {
                    return "ERR count must be in [1, 10000000]";
                }

                const std::optional<StaticModel::ModelFormat> format = ParseModelFormatArg(args[1]);
                if (!format) {
                    return "ERR format must be obj, gltf or fbx";
                }

                std::vector<uint8_t> pixels;
                UINT width = 0;
                UINT height = 0;
                if (!AssetLoader::LoadRgba8ViaWIC(ApplicationResourcePaths::ResolveAssetPathWide(args[2]), pixels, width, height)) {
                    return "ERR could not load density image " + args[2];
                }

                DensityScatterDesc desc{};
                desc.rgba     = pixels.data();
                desc.width    = width;
                desc.height   = height;
                desc.minX     = *numbers[1];
                desc.minZ     = *numbers[2];
                desc.maxX     = *numbers[3];
                desc.maxZ     = *numbers[4];
                desc.y        = optional(5, 0.0f);
                desc.count    = static_cast<uint32_t>(count);
                desc.seed     = static_cast<uint32_t>(optional(6, 0.0f));
                desc.scaleMin = optional(7, 1.0f);
                desc.scaleMax = optional(8, desc.scaleMin);
                if (!(desc.maxX > desc.minX) || !(desc.maxZ > desc.minZ)) {
                    return "ERR max must exceed min on both axes";
                }

                DensityScatterResult scattered = ScatterByDensity(desc);
                if (scattered.transforms.empty()) {
                    return "ERR no placement accepted (attempts=" + std::to_string(scattered.attempts) + ")";
                }

                const size_t placed = scattered.transforms.size();
                const std::string error = SpawnInstancedModel(app, args[0], *format, optional(9, 1.0f), std::move(scattered.transforms));
                if (!error.empty()) {
                    return error;
                }
                return "OK placed=" + std::to_string(placed) + "/" + std::to_string(desc.count)
                     + " attempts=" + std::to_string(scattered.attempts);
            });

        registry.Register("scene.spline",
            "<model> <obj|gltf|fbx> <spacing> <x0> <z0> <x1> <z1> [<x> <z> ...] [width=W] [seed=N] [y=Y] [scaleMin=S] [scaleMax=S]"
            " [yawJitter=rad] [uniformScale=S] - place instances every <spacing> m along a centripetal Catmull-Rom curve"
            " through the points, facing along it (local +Z); scene.save bakes them",
            [&app](const std::vector<std::string>& args) -> std::string {
                const std::string usage = "ERR usage: scene.spline <model> <obj|gltf|fbx> <spacing> <x0> <z0> <x1> <z1> [<x> <z> ...]"
                                          " [width=W] [seed=N] [y=Y] [scaleMin=S] [scaleMax=S] [yawJitter=rad] [uniformScale=S]";
                if (args.size() < 7) {
                    return usage;
                }

                const std::optional<StaticModel::ModelFormat> format = ParseModelFormatArg(args[1]);
                if (!format) {
                    return "ERR format must be obj, gltf or fbx";
                }

                SplinePlacementDesc desc{};
                float uniformScale = 1.0f;
                bool scaleMaxGiven = false;
                std::vector<float> coordinates;
                for (size_t i = 2; i < args.size(); ++i) {
                    const size_t eq = args[i].find('=');
                    const std::string key = eq == std::string::npos ? std::string() : args[i].substr(0, eq);
                    const std::optional<float> value = ParseFloat(eq == std::string::npos ? args[i] : args[i].substr(eq + 1));
                    if (!value) {
                        return "ERR not a number: " + args[i];
                    }
                    if (eq == std::string::npos) {
                        coordinates.push_back(*value);
                    } else if (key == "width") {
                        desc.width = *value;
                    } else if (key == "seed") {
                        desc.seed = static_cast<uint32_t>(*value);
                    } else if (key == "y") {
                        desc.y = *value;
                    } else if (key == "scaleMin") {
                        desc.scaleMin = *value;
                    } else if (key == "scaleMax") {
                        desc.scaleMax = *value;
                        scaleMaxGiven = true;
                    } else if (key == "yawJitter") {
                        desc.yawJitter = *value;
                    } else if (key == "uniformScale") {
                        uniformScale = *value;
                    } else {
                        return "ERR unknown option " + key;
                    }
                }
                if (!scaleMaxGiven) {
                    desc.scaleMax = desc.scaleMin;
                }

                // coordinates = spacing, then x/z pairs.
                if (coordinates.size() < 5 || (coordinates.size() - 1) % 2 != 0) {
                    return usage;
                }
                desc.spacing = coordinates[0];
                if (!(desc.spacing > 0.0f)) {
                    return "ERR spacing must be positive";
                }
                for (size_t i = 1; i + 1 < coordinates.size(); i += 2) {
                    desc.points.push_back({ coordinates[i], coordinates[i + 1] });
                }

                SplinePlacementResult placed = PlaceAlongSpline(desc);
                if (placed.transforms.empty()) {
                    return "ERR no placement produced (need 2 distinct points)";
                }

                const size_t count = placed.transforms.size();
                const std::string error = SpawnInstancedModel(app, args[0], *format, uniformScale, std::move(placed.transforms));
                if (!error.empty()) {
                    return error;
                }
                return "OK placed=" + std::to_string(count) + " length=" + FormatFloat(placed.length);
            });
    }
}
