#include "AppFramework/Debug/RemoteControl/DebugSceneCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/SpotLight.h"

#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace SasamiRenderer::Debug
{
    namespace
    {
        std::optional<float> ParseFloat(const std::string& text)
        {
            try {
                size_t consumed = 0;
                const float value = std::stof(text, &consumed);
                if (consumed != text.size()) {
                    return std::nullopt;
                }
                return value;
            } catch (...) {
                return std::nullopt;
            }
        }

        // Joins the remaining tokens back with single spaces, so a path may contain spaces
        // even though the registry splits purely on whitespace.
        std::string JoinFrom(const std::vector<std::string>& args, size_t first)
        {
            std::string joined;
            for (size_t i = first; i < args.size(); ++i) {
                if (!joined.empty()) {
                    joined += ' ';
                }
                joined += args[i];
            }
            return joined;
        }

        std::string FormatFloat(float value)
        {
            std::string text = std::to_string(value);
            // Trim trailing zeros so "1.000000" reads as "1".
            const size_t dot = text.find('.');
            if (dot != std::string::npos) {
                size_t last = text.find_last_not_of('0');
                if (last == dot) {
                    --last;
                }
                text.erase(last + 1);
            }
            return text;
        }
    }

    void RegisterSceneCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("ping", "connectivity check", [](const std::vector<std::string>&) {
            return std::string("pong");
        });

        registry.Register("camera.get", "current camera target and orientation (yaw/pitch returned in radians)",
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
                     + " pitch=" + FormatFloat(transform.rotation.pitch);
            });

        registry.Register("camera.setTarget", "<x> <y> <z> - move the camera target",
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

        registry.Register("camera.setDistance", "<d> - set orbit distance",
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
                return "OK";
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
    }
}
