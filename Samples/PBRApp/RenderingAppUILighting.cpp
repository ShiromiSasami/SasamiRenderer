// The "Lighting" tab was the largest single tab body in RegisterUI's ImGui panel, so it was
// split out of RenderingAppUI.cpp into this file to keep every UI file under the
// single-responsibility line-count threshold. RenderingAppUI.cpp still owns the UI::Tab that
// invokes DrawLightingTab, so the tab order stays readable from one place.

#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "UI/UIAngle.h"
#include "UI/UIButton.h"
#include "UI/UIColor.h"
#include "UI/UICommitPolicy.h"
#include "UI/UILayout.h"
#include "UI/UIText.h"
#include "UI/UIVector3.h"
#include "UI/UIVolume.h"
#include "Foundation/Math/MathUtil.h"
#include "imgui.h"

#include <cmath>
#include <cstdio>

namespace SasamiEngine::Sample::PBR
{
    void RenderingApp::DrawLightingTab(ApplicationCore& app)
    {
        // Lighting is the one place where the point of dragging is watching the scene
        // follow, so these parts report every frame instead of only on release.
        UI::CommitScope realtime{UI::CommitPolicy::Realtime};

        auto light = app.GetDirectionalLight();
        bool changed = false;
        changed |= UI::Angle("Yaw", &light.yaw, -180.0f, 180.0f);
        // Clamped to the range DirectionFromYawPitch is injective over, matching the
        // object rotation slider below. Allowing +-180 here let the sun be dragged past
        // vertical into a mirrored duplicate of the valid range, where it stops rising
        // and swings back down to the horizon instead.
        changed |= UI::Angle("Pitch", &light.pitch, -89.0f, 89.0f);
        changed |= UI::Volume("Distance", &light.distance, 0.1f, 50.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Shadow camera distance from the scene center.\nThis is not physical light falloff distance.");
        }
        changed |= UI::Volume("Ortho Half", &light.orthoHalf, 0.1f, 50.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Half-size of the directional shadow orthographic projection.\nLarger values cover more area but reduce shadow texel density.");
        }
        changed |= UI::Volume("Near", &light.nearZ, 0.01f, 100.0f);
        changed |= UI::Volume("Far", &light.farZ, 0.02f, 300.0f);
        changed |= UI::Color4("Dir Color", light.color.Data());
        changed |= UI::Volume("Dir Intensity", &light.intensity, 0.0f, 10.0f);
        int shadowMode = static_cast<int>(light.shadowMode);
        changed |= ImGui::RadioButton("Single Shadow", &shadowMode, static_cast<int>(DirectionalShadowMode::Single));
        changed |= ImGui::RadioButton("CSM4", &shadowMode, static_cast<int>(DirectionalShadowMode::Csm4));
        changed |= ImGui::RadioButton("VSM", &shadowMode, static_cast<int>(DirectionalShadowMode::Vsm));
        changed |= ImGui::RadioButton("VSM4", &shadowMode, static_cast<int>(DirectionalShadowMode::Vsm4));
        light.shadowMode = static_cast<DirectionalShadowMode>(shadowMode);
        if (shadowMode >= static_cast<int>(DirectionalShadowMode::Vsm)) {
            bool vsmBlur = app.GetVsmBlurEnabled();
            if (ImGui::Checkbox("VSM Blur", &vsmBlur)) {
                app.SetVsmBlurEnabled(vsmBlur);
            }
        }
        changed |= UI::Volume("Shadow Distance", &light.shadowDistance, 5.0f, 250.0f);
        changed |= UI::Volume("Cascade Exponent", &light.cascadeDistributionExponent, 1.0f, 4.0f);
        changed |= UI::Volume("Cascade Blend", &light.cascadeBlendFraction, 0.0f, 0.3f);
        changed |= UI::Volume("Depth Bias", &light.depthBias, 0.0f, 4000.0f);
        changed |= UI::Volume("Slope Bias", &light.slopeScaleBias, 0.0f, 8.0f);
        changed |= UI::Volume("Normal Bias", &light.normalBias, 0.0f, 0.1f);
        changed |= UI::Volume("Far Bias Scale", &light.farBiasScale, 1.0f, 4.0f);

        if (light.nearZ < 0.001f) {
            light.nearZ = 0.001f;
            changed = true;
        }
        if (light.farZ <= light.nearZ + 0.01f) {
            light.farZ = light.nearZ + 0.02f;
            changed = true;
        }
        if (light.shadowDistance < 1.0f) {
            light.shadowDistance = 1.0f;
            changed = true;
        }
        if (changed) {
            app.SetDirectionalLight(light);
        }

        UI::Separator();
        ImGui::Checkbox("Show Light Gizmo", &m_showLightGizmo);


        bool showDirectionalLightOnSkybox = app.GetShowDirectionalLightOnSkybox();
        if (ImGui::Checkbox("Show Dir On Skybox", &showDirectionalLightOnSkybox)) {
            app.SetShowDirectionalLightOnSkybox(showDirectionalLightOnSkybox);
        }
        if (showDirectionalLightOnSkybox) {
            float directionalLightOnSkyboxAngularRadius = app.GetDirectionalLightOnSkyboxAngularRadius();
            if (UI::Angle("Skybox Marker Size",
                                   &directionalLightOnSkyboxAngularRadius,
                                   0.1f,
                                   15.0f)) {
                app.SetDirectionalLightOnSkyboxAngularRadius(directionalLightOnSkyboxAngularRadius);
            }
        }

        UI::Separator();
        auto pointLights = app.GetPointLightObjects();
        UI::Text("Point Lights: %d", static_cast<int>(pointLights.size()));
        if (UI::Button("Add Point")) {
            app.CreatePointLightObject();
            pointLights = app.GetPointLightObjects();
        }
        ImGui::SameLine();
        if (UI::Button("Remove Point") && !pointLights.empty()) {
            app.DeleteObject(pointLights.back());
            pointLights = app.GetPointLightObjects();
        }
        for (size_t i = 0; i < pointLights.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            UI::Text("Point %d", static_cast<int>(i));
            auto* pl = pointLights[i];
            if (!pl) {
                ImGui::PopID();
                continue;
            }
            auto& tr = pl->Transform();
            float pos[3] = { tr.position.x, tr.position.y, tr.position.z };
            if (UI::Vector3("Pos", pos, 0.05f)) {
                tr.position.x = pos[0];
                tr.position.y = pos[1];
                tr.position.z = pos[2];
            }
            float* plColor = pl->ColorData();
            float color[4] = { plColor[0], plColor[1], plColor[2], plColor[3] };
            if (UI::Color4("Color", color)) {
                plColor[0] = color[0];
                plColor[1] = color[1];
                plColor[2] = color[2];
                plColor[3] = color[3];
            }
            float intensity = pl->Intensity();
            if (UI::Volume("Intensity", &intensity, 0.0f, 10.0f)) {
                pl->SetIntensity(intensity);
            }
            float range = pl->Range();
            if (UI::Volume("Range", &range, 0.1f, 50.0f)) {
                if (range < 0.01f) {
                    range = 0.01f;
                }
                pl->SetRange(range);
            }
            ImGui::PopID();
        }

        UI::Separator();
        auto spotLights = app.GetSpotLightObjects();
        UI::Text("Spot Lights: %d", static_cast<int>(spotLights.size()));
        if (UI::Button("Add Spot")) {
            app.CreateSpotLightObject();
            spotLights = app.GetSpotLightObjects();
        }
        ImGui::SameLine();
        if (UI::Button("Remove Spot") && !spotLights.empty()) {
            app.DeleteObject(spotLights.back());
            spotLights = app.GetSpotLightObjects();
        }
        for (size_t i = 0; i < spotLights.size(); ++i) {
            ImGui::PushID(100 + static_cast<int>(i));
            UI::Text("Spot %d", static_cast<int>(i));
            auto* sl = spotLights[i];
            if (!sl) {
                ImGui::PopID();
                continue;
            }
            auto& tr = sl->Transform();
            float pos[3] = { tr.position.x, tr.position.y, tr.position.z };
            if (UI::Vector3("Pos", pos, 0.05f)) {
                tr.position.x = pos[0];
                tr.position.y = pos[1];
                tr.position.z = pos[2];
            }
            float yaw = tr.rotation.yaw;
            if (UI::Angle("Yaw", &yaw, -180.0f, 180.0f)) {
                tr.rotation.yaw = yaw;
            }
            float pitch = tr.rotation.pitch;
            if (UI::Angle("Pitch", &pitch, -89.0f, 89.0f)) {
                tr.rotation.pitch = pitch;
            }
            float* slColor = sl->ColorData();
            float color[4] = { slColor[0], slColor[1], slColor[2], slColor[3] };
            if (UI::Color4("Color", color)) {
                slColor[0] = color[0];
                slColor[1] = color[1];
                slColor[2] = color[2];
                slColor[3] = color[3];
            }

            float intensity = sl->Intensity();
            if (UI::Volume("Intensity", &intensity, 0.0f, 10.0f)) {
                sl->SetIntensity(intensity);
            }
            float range = sl->Range();
            if (UI::Volume("Range", &range, 0.1f, 50.0f)) {
                if (range < 0.01f) {
                    range = 0.01f;
                }
                sl->SetRange(range);
            }

            float innerAngle = sl->InnerAngle();
            float outerAngle = sl->OuterAngle();
            bool angleChanged = false;
            angleChanged |= UI::Angle("Inner", &innerAngle, 0.0f, 89.0f);
            angleChanged |= UI::Angle("Outer", &outerAngle, 0.0f, 89.0f);
            if (innerAngle > outerAngle) {
                innerAngle = outerAngle;
                angleChanged = true;
            }
            if (angleChanged) {
                sl->SetInnerAngle(innerAngle);
                sl->SetOuterAngle(outerAngle);
            }
            ImGui::PopID();
        }

        // ── Point / Spot light 3D gizmos (viewport overlay) ──────────────
        UI::Separator();
        ImGui::Checkbox("Show Point/Spot Gizmos", &m_showLightGizmos);
        if (m_showLightGizmos) {
            const auto& gProxy = app.GetMainCameraProxy();
            const float* gvp   = gProxy.viewProjection;
            const float  gsw   = static_cast<float>(app.GetWidth());
            const float  gsh   = static_cast<float>(app.GetHeight());

            // World → screen (row-vector: clip = worldPos * VP)
            auto gw2s = [&](float wx, float wy, float wz, ImVec2& out) -> bool {
                const float cx = wx*gvp[0] + wy*gvp[4] + wz*gvp[8]  + gvp[12];
                const float cy = wx*gvp[1] + wy*gvp[5] + wz*gvp[9]  + gvp[13];
                const float cw = wx*gvp[3] + wy*gvp[7] + wz*gvp[11] + gvp[15];
                if (std::abs(cw) < 1e-7f || cw < 0.0f) return false;
                const float ndcX = cx / cw, ndcY = cy / cw;
                out = { (ndcX + 1.f) * 0.5f * gsw, (1.f - ndcY) * 0.5f * gsh };
                return ndcX >= -1.1f && ndcX <= 1.1f && ndcY >= -1.1f && ndcY <= 1.1f;
            };

            // Background draw list, not the foreground one: ImGui renders the foreground
            // list after every window, so scene gizmos drawn there sit on top of the
            // control panel. The background list is drawn before windows, which keeps
            // the UI clickable and visually in front.
            ImDrawList* gdl = ImGui::GetBackgroundDrawList();

            // ── Point lights: filled circle + crosshair + label
            for (size_t pi = 0; pi < pointLights.size(); ++pi) {
                auto* pl = pointLights[pi];
                if (!pl) continue;
                const auto& ppos = pl->Transform().position;
                ImVec2 psc{};
                if (!gw2s(ppos.x, ppos.y, ppos.z, psc)) continue;

                const float* pc = pl->ColorData();
                const ImU32 pcol   = IM_COL32(static_cast<int>(std::fmin(pc[0],1.f)*255.f),
                                               static_cast<int>(std::fmin(pc[1],1.f)*255.f),
                                               static_cast<int>(std::fmin(pc[2],1.f)*255.f), 220);
                const ImU32 pcolBg = IM_COL32(static_cast<int>(std::fmin(pc[0],1.f)*160.f),
                                               static_cast<int>(std::fmin(pc[1],1.f)*160.f),
                                               static_cast<int>(std::fmin(pc[2],1.f)*160.f), 130);
                constexpr float kPR = 10.f;
                gdl->AddCircleFilled(psc, kPR, pcolBg, 16);
                gdl->AddCircle(psc, kPR, pcol, 16, 2.f);
                gdl->AddLine({psc.x - kPR*0.5f, psc.y}, {psc.x + kPR*0.5f, psc.y}, pcol, 1.5f);
                gdl->AddLine({psc.x, psc.y - kPR*0.5f}, {psc.x, psc.y + kPR*0.5f}, pcol, 1.5f);
                char pbuf[16];
                std::snprintf(pbuf, sizeof(pbuf), "P%d", static_cast<int>(pi));
                gdl->AddText({psc.x + kPR + 3.f, psc.y - 7.f}, pcol, pbuf);
            }

            // ── Spot lights: diamond + direction arrow + cone edges + label
            for (size_t si = 0; si < spotLights.size(); ++si) {
                auto* sl = spotLights[si];
                if (!sl) continue;
                const auto& str = sl->Transform();
                const auto& spos = str.position;
                ImVec2 ssc{};
                if (!gw2s(spos.x, spos.y, spos.z, ssc)) continue;

                const float* sc_ = sl->ColorData();
                const ImU32 scol   = IM_COL32(static_cast<int>(std::fmin(sc_[0],1.f)*255.f),
                                               static_cast<int>(std::fmin(sc_[1],1.f)*255.f),
                                               static_cast<int>(std::fmin(sc_[2],1.f)*255.f), 220);
                const ImU32 scolBg = IM_COL32(static_cast<int>(std::fmin(sc_[0],1.f)*160.f),
                                               static_cast<int>(std::fmin(sc_[1],1.f)*160.f),
                                               static_cast<int>(std::fmin(sc_[2],1.f)*160.f), 130);
                constexpr float kSR = 10.f;

                // Diamond icon
                const ImVec2 dtop  = {ssc.x,        ssc.y - kSR};
                const ImVec2 drt   = {ssc.x + kSR,  ssc.y};
                const ImVec2 dbot  = {ssc.x,        ssc.y + kSR};
                const ImVec2 dlt   = {ssc.x - kSR,  ssc.y};
                gdl->AddQuadFilled(dtop, drt, dbot, dlt, scolBg);
                gdl->AddQuad(dtop, drt, dbot, dlt, scol, 2.f);

                char sbuf[16];
                std::snprintf(sbuf, sizeof(sbuf), "S%d", static_cast<int>(si));
                gdl->AddText({ssc.x + kSR + 3.f, ssc.y - 7.f}, scol, sbuf);

                // Forward direction from yaw/pitch
                float sDir[3] = {};
                Math::DirectionFromYawPitch(str.rotation.yaw, str.rotation.pitch, sDir);
                const float sfx = sDir[0], sfy = sDir[1], sfz = sDir[2];

                // Direction arrow
                const float arrowDist = std::fmin(sl->Range() * 0.5f, 3.0f);
                ImVec2 ae{};
                if (gw2s(spos.x + sfx*arrowDist, spos.y + sfy*arrowDist, spos.z + sfz*arrowDist, ae)) {
                    gdl->AddLine(ssc, ae, scol, 2.f);
                    float adx = ae.x - ssc.x, ady = ae.y - ssc.y;
                    const float alen = std::sqrt(adx*adx + ady*ady);
                    if (alen > 1.f) {
                        adx /= alen; ady /= alen;
                        const ImVec2 ah1 = {ae.x - adx*8.f + ady*4.f, ae.y - ady*8.f - adx*4.f};
                        const ImVec2 ah2 = {ae.x - adx*8.f - ady*4.f, ae.y - ady*8.f + adx*4.f};
                        gdl->AddTriangleFilled(ae, ah1, ah2, scol);
                    }
                }

                // Cone edges (outer angle, 4 spokes: right/left/up/down)
                float upX = 0.f, upY = 1.f, upZ = 0.f;
                if (std::fabs(sfy) > 0.99f) { upX = 1.f; upY = 0.f; upZ = 0.f; }
                float rX = sfy*upZ - sfz*upY, rY = sfz*upX - sfx*upZ, rZ = sfx*upY - sfy*upX;
                const float rlen = std::sqrt(rX*rX + rY*rY + rZ*rZ);
                if (rlen < 1e-6f) continue;
                rX /= rlen; rY /= rlen; rZ /= rlen;
                const float uX = sfy*rZ - sfz*rY, uY = sfz*rX - sfx*rZ, uZ = sfx*rY - sfy*rX;

                const float coneLen = std::fmin(sl->Range(), 3.0f);
                const float cosO = std::cos(sl->OuterAngle()), sinO = std::sin(sl->OuterAngle());
                const float edgeDirs[4][3] = {
                    { sfx*cosO + rX*sinO, sfy*cosO + rY*sinO, sfz*cosO + rZ*sinO },
                    { sfx*cosO - rX*sinO, sfy*cosO - rY*sinO, sfz*cosO - rZ*sinO },
                    { sfx*cosO + uX*sinO, sfy*cosO + uY*sinO, sfz*cosO + uZ*sinO },
                    { sfx*cosO - uX*sinO, sfy*cosO - uY*sinO, sfz*cosO - uZ*sinO },
                };
                for (int e = 0; e < 4; ++e) {
                    ImVec2 ept{};
                    if (gw2s(spos.x + edgeDirs[e][0]*coneLen,
                             spos.y + edgeDirs[e][1]*coneLen,
                             spos.z + edgeDirs[e][2]*coneLen, ept)) {
                        gdl->AddLine(ssc, ept, scol, 1.5f);
                    }
                }
            }
        }
        // ── End Point / Spot gizmos ───────────────────────────────────────
    }
}
