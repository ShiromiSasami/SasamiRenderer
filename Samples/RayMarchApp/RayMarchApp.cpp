#include "RayMarchApp.h"
#include "ApplicationCore.h"
#include "ApplicationEntryPoint.h"
#include "Input/InputSystem.h"
#include "Renderer/Passes/Core/RenderNode.h"
#include "Renderer/Passes/PostProcess/PostProcessRenderPass.h"
#include "UI/ImGuiCoordinator.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "imgui.h"

#include <windows.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <boost/signals2/connection.hpp>

namespace SasamiRenderer
{
    void RayMarchApp::OnInit(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("RayMarchApp: Camera object creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("RayMarchApp: SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }

        m_camera->SetTarget(m_camX, m_camY, m_camZ);
        m_camera->SetYawPitch(m_yaw, m_pitch);
        m_camera->SetMoveSpeed(5.0f);
        m_camera->SetClipPlanes(0.0005f, 500.0f);
        m_camera->SetCameraMode(Camera::CameraMode::RayMarch);

        // Replace the default render node preset with RayMarch and the final
        // SceneColor -> BackBuffer post process pass.
        m_renderPass = std::make_shared<RayMarchRenderPass>();
        std::vector<std::shared_ptr<IRenderPass>> rayMarchPasses;
        rayMarchPasses.push_back(m_renderPass);
        rayMarchPasses.push_back(std::make_shared<PostProcessRenderPass>());
        app.SetRenderNodePreset(std::make_shared<RenderPassSequenceNode>("RayMarchRenderNode",
                                                                         std::move(rayMarchPasses)));
        // Set up directional light (sun)
        DirectionalLight sun{};
        sun.yaw       = 45.0f;
        sun.pitch     = 40.0f;
        sun.intensity = 2.5f;
        sun.color.r = 1.00f;
        sun.color.g = 0.95f;
        sun.color.b = 0.85f;
        app.SetDirectionalLight(sun);

        // Input bindings
        auto& input = InputSystem::Instance();
        static std::vector<boost::signals2::scoped_connection> s_connections;
        s_connections.clear();

        s_connections.emplace_back(input.ConnectOnKeyDown([this](WPARAM k) {
            if (m_camera) m_camera->OnKeyDown(k);
        }));
        s_connections.emplace_back(input.ConnectOnKeyUp([this](WPARAM k) {
            if (m_camera) m_camera->OnKeyUp(k);
        }));
        s_connections.emplace_back(input.ConnectOnMouseDown([this](int x, int y) {
            if (m_camera) m_camera->OnMouseDown(x, y);
        }));
        s_connections.emplace_back(input.ConnectOnMouseUp([this]() {
            if (m_camera) m_camera->OnMouseUp();
        }));
        s_connections.emplace_back(input.ConnectOnMouseMove([this](int x, int y, bool held) {
            if (m_camera) m_camera->OnMouseMove(x, y, held);
        }));
        s_connections.emplace_back(input.ConnectOnMouseWheel([this](int d) {
            if (m_camera) m_camera->OnMouseWheel(d);
        }));

        // UI window
        ImGuiCoordinator::Instance().RegisterWindow("Camera", [this]() {
            if (!m_camera) { ImGui::TextDisabled("Camera unavailable."); return; }

            // FPS
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

            // Camera position
            const auto& pos = m_camera->Transform().position;
            ImGui::Text("Pos: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);

            ImGui::Separator();

            float speed = m_camera->MoveSpeed();
            if (ImGui::SliderFloat("Move Speed", &speed, 0.1f, 30.0f)) {
                m_camera->SetMoveSpeed(speed);
            }

            // ── Cloud settings ────────────────────────────────────────────
            ImGui::Separator();
            if (m_renderPass) {
                float cloudCover = m_renderPass->GetCloudCover();
                if (ImGui::SliderFloat("Cloud Cover", &cloudCover, 0.0f, 1.0f)) {
                    m_renderPass->SetCloudCover(cloudCover);
                }
                float cloudDensity = m_renderPass->GetCloudDensity();
                if (ImGui::SliderFloat("Cloud Density", &cloudDensity, 0.5f, 8.0f)) {
                    m_renderPass->SetCloudDensity(cloudDensity);
                }
            }

            // ── Distance Heatmap Debug ────────────────────────────────────
            ImGui::Separator();
            if (m_renderPass) {
                bool heatmap = (m_renderPass->GetDebugMode() > 0.5f);
                if (ImGui::Checkbox("Distance Heatmap", &heatmap))
                    m_renderPass->SetDebugMode(heatmap ? 1.0f : 0.0f);
                if (heatmap)
                    ImGui::TextDisabled("blue=near  green=mid  red=far(300m)  white=sky");
            }

            // ── waveLod Weights ───────────────────────────────────────────
            ImGui::Separator();
            ImGui::TextDisabled("-- waveLod Weights --");
            ImGui::TextDisabled("dist(m)  midW   hiW");
            constexpr float kRefDist[] = { 10.f, 30.f, 50.f, 80.f, 120.f, 160.f, 240.f, 320.f };
            for (float d : kRefDist) {
                float midW = (std::max)(0.f, (std::min)(1.f, 1.f - (d - 160.f) / 160.f));
                float hiW  = (std::max)(0.f, (std::min)(1.f, 1.f - (d -  80.f) /  80.f));
                bool hiGone  = (hiW  < 0.01f);
                bool midGone = (midW < 0.01f);
                if      (midGone) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
                else if (hiGone)  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
                ImGui::Text(" %5.0f   %.3f  %.3f", d, midW, hiW);
                if (hiGone || midGone) ImGui::PopStyleColor();
            }
            ImGui::TextDisabled("yellow=hiW gone  orange=both gone");

            // ── Cone March Debug ──────────────────────────────────────────
            ImGui::Separator();
            ImGui::TextDisabled("-- Cone March Debug --");
            ImGui::Text("pixelConeAngle : %.6f rad", m_dbgPixelConeAngle);
            ImGui::Text("kEps           : 0.0005 m");
            ImGui::Separator();
            ImGui::TextDisabled(" t(m)  epsilon(m)  > hi_wave?");
            // hi_wave amplitude = 0.012m (smallest wave octave)
            constexpr float kWaveHi  = 0.012f;
            constexpr float kWaveMid = 0.048f;
            constexpr float kEps     = 0.0005f;
            constexpr float kRefT[]  = { 1.f, 5.f, 10.f, 25.f, 50.f, 100.f, 200.f };
            for (float t : kRefT) {
                float eps = (std::max)(m_dbgPixelConeAngle * t, kEps);
                bool overHi  = eps > kWaveHi;
                bool overMid = eps > kWaveMid;
                if (overMid)      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                else if (overHi)  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
                ImGui::Text(" %5.0f   %.5f   %s",
                    t, eps,
                    overMid ? "> mid_wave!" : (overHi ? "> hi_wave" : "ok"));
                if (overHi) ImGui::PopStyleColor();
            }
            ImGui::Separator();
            ImGui::TextDisabled("wave amp: hi=0.012 mid=0.048 lo=0.090-0.180");
            ImGui::TextDisabled("waveLod hiW  fades: 80-160m from cam");
            ImGui::TextDisabled("waveLod midW fades: 160-320m from cam");

        });
    }

    void RayMarchApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        if (m_camera) {
            m_camera->Update(deltaTime);

            // ── Cone march debug: replicate shader's pixelConeAngle on CPU ──
            // Shader computes: adjFar4 = mul(float4(ndc.x, ndc.y + 2/h, 1, 1), invVP)
            //                  adjRd   = normalize(adjFar4.xyz/w - nearWS)
            //                  pixelConeAngle = length(rd - adjRd)
            // We reproduce the same calculation here using the camera's VP matrix.
            const UINT w = app.GetWidth();
            const UINT h = app.GetHeight();
            if (w > 0 && h > 0) {
                auto vp = m_camera->ComputeMVP((float)w, (float)h);
                float invVP[16];
                Math::Invert4x4(vp.data(), invVP);

                // Row-vector multiply: out = (nx,ny,nz,nw) * invVP
                auto mul4 = [&](float nx, float ny, float nz, float nw,
                                float& ox, float& oy, float& oz, float& ow) {
                    ox = nx*invVP[0]  + ny*invVP[4]  + nz*invVP[8]  + nw*invVP[12];
                    oy = nx*invVP[1]  + ny*invVP[5]  + nz*invVP[9]  + nw*invVP[13];
                    oz = nx*invVP[2]  + ny*invVP[6]  + nz*invVP[10] + nw*invVP[14];
                    ow = nx*invVP[3]  + ny*invVP[7]  + nz*invVP[11] + nw*invVP[15];
                };

                // nearWS = near4.xyz / near4.w  (NDC z=0)
                float nx, ny, nz, nw;
                mul4(0.f, 0.f, 0.f, 1.f, nx, ny, nz, nw);
                float nearX = nx/nw, nearY = ny/nw, nearZ = nz/nw;

                // Center ray: NDC (0,0,1,1)
                float fx, fy, fz, fw;
                mul4(0.f, 0.f, 1.f, 1.f, fx, fy, fz, fw);
                float rcx = fx/fw - nearX, rcy = fy/fw - nearY, rcz = fz/fw - nearZ;
                float rcLen = std::sqrt(rcx*rcx + rcy*rcy + rcz*rcz);
                rcx /= rcLen; rcy /= rcLen; rcz /= rcLen;

                // Adjacent pixel ray: NDC (0, +2/h, 1, 1)
                float ax, ay, az, aw;
                mul4(0.f, 2.0f/(float)h, 1.f, 1.f, ax, ay, az, aw);
                float rax = ax/aw - nearX, ray_ = ay/aw - nearY, raz = az/aw - nearZ;
                float raLen = std::sqrt(rax*rax + ray_*ray_ + raz*raz);
                rax /= raLen; ray_ /= raLen; raz /= raLen;

                float dx = rcx-rax, dy = rcy-ray_, dz = rcz-raz;
                m_dbgPixelConeAngle = (std::max)(std::sqrt(dx*dx + dy*dy + dz*dz), 1e-5f);
            }
        }
    }

    void RayMarchApp::OnRender(ApplicationCore& app)
    {
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void RayMarchApp::OnShutdown(ApplicationCore& app)
    {
        app.ClearObjects();
        m_camera     = nullptr;
        m_renderPass = nullptr;
    }

    void RayMarchApp::OnResize(ApplicationCore& app, UINT width, UINT height)
    {
        (void)app;
        (void)width;
        (void)height;
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiRenderer::RayMarchApp, 1280, 720, L"RayMarch Sample")
