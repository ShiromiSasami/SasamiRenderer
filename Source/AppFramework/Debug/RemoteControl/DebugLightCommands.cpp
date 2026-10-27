#include "AppFramework/Debug/RemoteControl/DebugLightCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Light/DirectionalLight.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
        constexpr float kRadToDeg = 180.0f / std::numbers::pi_v<float>;
    }

    // Every setter below reads the value back from ApplicationCore after writing it, rather than
    // echoing the value the caller requested, because SetDirectionalLight may clamp or reject
    // parts of the request. Reporting the read-back keeps the remote caller from believing a
    // rejected change actually applied (same rationale as DebugRenderCommands.cpp).
    void RegisterLightCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("light.get", "current directional light state",
            [&app](const std::vector<std::string>&) -> std::string {
                const auto light = app.GetDirectionalLight();
                return "OK yawDeg=" + FormatFloat(light.yaw * kRadToDeg)
                     + " pitchDeg=" + FormatFloat(light.pitch * kRadToDeg)
                     + " yaw=" + FormatFloat(light.yaw)
                     + " pitch=" + FormatFloat(light.pitch)
                     + " intensity=" + FormatFloat(light.intensity)
                     + " distance=" + FormatFloat(light.distance)
                     + " orthoHalf=" + FormatFloat(light.orthoHalf)
                     + " nearZ=" + FormatFloat(light.nearZ)
                     + " farZ=" + FormatFloat(light.farZ)
                     + " shadowDistance=" + FormatFloat(light.shadowDistance)
                     + " depthBias=" + FormatFloat(light.depthBias)
                     + " slopeScaleBias=" + FormatFloat(light.slopeScaleBias)
                     + " normalBias=" + FormatFloat(light.normalBias)
                     + " farBiasScale=" + FormatFloat(light.farBiasScale)
                     + " contactShadow=" + std::string(light.contactShadowEnabled ? "1" : "0")
                     + " cascadeExponent=" + FormatFloat(light.cascadeDistributionExponent)
                     + " cascadeBlend=" + FormatFloat(light.cascadeBlendFraction);
            });

        // Arguments are DEGREES even though DirectionalLight stores yaw/pitch in radians, because
        // sun angles are naturally reasoned about in degrees. Camera::SetYawPitch (the analogous
        // camera command) takes radians, so both the help text and the reply echo degrees *and*
        // radians together to avoid unit mix-ups between the two commands.
        registry.Register("light.dir", "<yawDeg> <pitchDeg> - set sun direction in DEGREES (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    const auto light = app.GetDirectionalLight();
                    return "OK yawDeg=" + FormatFloat(light.yaw * kRadToDeg)
                         + " pitchDeg=" + FormatFloat(light.pitch * kRadToDeg)
                         + " yaw=" + FormatFloat(light.yaw)
                         + " pitch=" + FormatFloat(light.pitch);
                }
                if (args.size() != 2) {
                    return "ERR usage: light.dir <yawDeg> <pitchDeg>";
                }
                const auto yawDeg = ParseFloat(args[0]);
                const auto pitchDeg = ParseFloat(args[1]);
                if (!yawDeg || !pitchDeg) {
                    return "ERR usage: light.dir <yawDeg> <pitchDeg>";
                }

                auto light = app.GetDirectionalLight();
                light.yaw = *yawDeg * kDegToRad;
                light.pitch = *pitchDeg * kDegToRad;
                app.SetDirectionalLight(light);

                const auto readBack = app.GetDirectionalLight();
                return "OK yawDeg=" + FormatFloat(readBack.yaw * kRadToDeg)
                     + " pitchDeg=" + FormatFloat(readBack.pitch * kRadToDeg)
                     + " yaw=" + FormatFloat(readBack.yaw)
                     + " pitch=" + FormatFloat(readBack.pitch);
            });

        registry.Register("light.intensity", "<f> - set directional light intensity (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK intensity=" + FormatFloat(app.GetDirectionalLight().intensity);
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value < 0.0f) {
                    return "ERR intensity must be >= 0";
                }

                auto light = app.GetDirectionalLight();
                light.intensity = *value;
                app.SetDirectionalLight(light);
                return "OK intensity=" + FormatFloat(app.GetDirectionalLight().intensity);
            });

        registry.Register("light.ortho", "<f> - set shadow ortho half-size (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK orthoHalf=" + FormatFloat(app.GetDirectionalLight().orthoHalf);
                }
                const auto value = ParseFloat(args[0]);
                if (!value || *value <= 0.0f) {
                    return "ERR orthoHalf must be > 0";
                }

                auto light = app.GetDirectionalLight();
                light.orthoHalf = *value;
                app.SetDirectionalLight(light);
                return "OK orthoHalf=" + FormatFloat(app.GetDirectionalLight().orthoHalf);
            });

        registry.Register("light.range", "<nearZ> <farZ> - set shadow projection near/far (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    const auto light = app.GetDirectionalLight();
                    return "OK nearZ=" + FormatFloat(light.nearZ) + " farZ=" + FormatFloat(light.farZ);
                }
                if (args.size() != 2) {
                    return "ERR usage: light.range <nearZ> <farZ>";
                }
                const auto nearZ = ParseFloat(args[0]);
                const auto farZ = ParseFloat(args[1]);
                if (!nearZ || !farZ || *nearZ <= 0.0f || *farZ <= *nearZ) {
                    return "ERR require 0 < nearZ < farZ";
                }

                auto light = app.GetDirectionalLight();
                light.nearZ = *nearZ;
                light.farZ = *farZ;
                app.SetDirectionalLight(light);

                const auto readBack = app.GetDirectionalLight();
                return "OK nearZ=" + FormatFloat(readBack.nearZ) + " farZ=" + FormatFloat(readBack.farZ);
            });

        // Contact shadows are multiplied into the same directional visibility term as the
        // cascade lookup, so a screen-space artefact and a shadow-map artefact are
        // indistinguishable until one of them can be switched off.
        registry.Register("light.contactshadow", "<0|1> - screen-space contact shadows for the directional light (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                auto light = app.GetDirectionalLight();
                if (args.empty()) {
                    return std::string("OK contactShadow=") + (light.contactShadowEnabled ? "1" : "0");
                }
                const auto value = ParseFloat(args[0]);
                if (!value || (*value != 0.0f && *value != 1.0f)) {
                    return "ERR usage: light.contactshadow <0|1>";
                }
                light.contactShadowEnabled = (*value != 0.0f);
                app.SetDirectionalLight(light);
                return std::string("OK contactShadow=") + (light.contactShadowEnabled ? "1" : "0");
            });

        registry.Register("light.bias", "<depthBias> [slopeScaleBias] [normalBias] - set shadow depth bias (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    const auto light = app.GetDirectionalLight();
                    return "OK depthBias=" + FormatFloat(light.depthBias)
                         + " slopeScaleBias=" + FormatFloat(light.slopeScaleBias)
                         + " normalBias=" + FormatFloat(light.normalBias);
                }
                if (args.size() > 3) {
                    return "ERR usage: light.bias <depthBias> [slopeScaleBias] [normalBias]";
                }
                const auto depthBias = ParseFloat(args[0]);
                if (!depthBias) {
                    return "ERR usage: light.bias <depthBias> [slopeScaleBias] [normalBias]";
                }
                std::optional<float> slopeScaleBias;
                if (args.size() > 1) {
                    slopeScaleBias = ParseFloat(args[1]);
                    if (!slopeScaleBias) {
                        return "ERR usage: light.bias <depthBias> [slopeScaleBias] [normalBias]";
                    }
                }
                std::optional<float> normalBias;
                if (args.size() > 2) {
                    normalBias = ParseFloat(args[2]);
                    if (!normalBias) {
                        return "ERR usage: light.bias <depthBias> [slopeScaleBias] [normalBias]";
                    }
                }

                auto light = app.GetDirectionalLight();
                light.depthBias = *depthBias;
                if (slopeScaleBias) {
                    light.slopeScaleBias = *slopeScaleBias;
                }
                if (normalBias) {
                    light.normalBias = *normalBias;
                }
                app.SetDirectionalLight(light);

                const auto readBack = app.GetDirectionalLight();
                return "OK depthBias=" + FormatFloat(readBack.depthBias)
                     + " slopeScaleBias=" + FormatFloat(readBack.slopeScaleBias)
                     + " normalBias=" + FormatFloat(readBack.normalBias);
            });

        registry.Register("light.cascade", "<exponent> [blendFraction] - set cascade split exponent and blend (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    const auto light = app.GetDirectionalLight();
                    return "OK cascadeExponent=" + FormatFloat(light.cascadeDistributionExponent)
                         + " cascadeBlend=" + FormatFloat(light.cascadeBlendFraction);
                }
                if (args.size() > 2) {
                    return "ERR usage: light.cascade <exponent> [blendFraction]";
                }
                const auto exponent = ParseFloat(args[0]);
                if (!exponent || *exponent <= 0.0f) {
                    return "ERR exponent must be > 0";
                }
                std::optional<float> blendFraction;
                if (args.size() > 1) {
                    blendFraction = ParseFloat(args[1]);
                    if (!blendFraction || *blendFraction < 0.0f || *blendFraction > 1.0f) {
                        return "ERR blendFraction must be in 0..1";
                    }
                }

                auto light = app.GetDirectionalLight();
                light.cascadeDistributionExponent = *exponent;
                if (blendFraction) {
                    light.cascadeBlendFraction = *blendFraction;
                }
                app.SetDirectionalLight(light);

                const auto readBack = app.GetDirectionalLight();
                return "OK cascadeExponent=" + FormatFloat(readBack.cascadeDistributionExponent)
                     + " cascadeBlend=" + FormatFloat(readBack.cascadeBlendFraction);
            });
    }
}
