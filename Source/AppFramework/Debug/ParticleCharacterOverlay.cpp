#include "AppFramework/Debug/ParticleCharacterOverlay.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/WindOverlayProjection.h"
#include "AppFramework/Object/Camera.h"
#include "Renderer/Scene/RenderCameraProxy.h"
#include "imgui.h"
#include "Foundation/Tools/ScopedPerfTimer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        constexpr float MAX_STEP_DT = 1.0f / 20.0f;
        constexpr float POINT_WORLD_SIZE = 0.012f;  // world size of one particle square, clamped in pixels below
        constexpr float MIN_POINT_PIXELS = 1.0f;
        constexpr float MAX_POINT_PIXELS = 4.0f;

        using Foundation::MillisecondsSince;

        // Bonded = warm orange, free = cool blue.
        ImU32 BondColor(float bond)
        {
            const float b = (std::min)((std::max)(bond, 0.0f), 1.0f);
            const float r = 0.35f + 0.65f * b;
            const float g = 0.55f + 0.15f * b;
            const float bl = 1.0f - 0.75f * b;
            return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f), static_cast<int>(bl * 255.0f), 230);
        }
    }

    const char* MolecularForceName(Render::ParticleCharacter::MolecularForce force)
    {
        switch (force) {
        case Render::ParticleCharacter::MolecularForce::Swirl: return "swirl";
        case Render::ParticleCharacter::MolecularForce::Melt: return "melt";
        default: return "scatter";
        }
    }

    bool ParseMolecularForce(const std::string& text, Render::ParticleCharacter::MolecularForce& out)
    {
        if (text == "scatter") { out = Render::ParticleCharacter::MolecularForce::Scatter; return true; }
        if (text == "swirl") { out = Render::ParticleCharacter::MolecularForce::Swirl; return true; }
        if (text == "melt") { out = Render::ParticleCharacter::MolecularForce::Melt; return true; }
        return false;
    }

    ParticleCharacterOverlay::ParticleCharacterOverlay() = default;
    ParticleCharacterOverlay::~ParticleCharacterOverlay() = default;

    std::string ParticleCharacterOverlay::Start(const ParticleCharacterViewSettings& settings)
    {
        Stop();
        if (!(settings.height > 0.0f) || settings.count == 0) {
            return "ERR height and count must be positive";
        }
        auto model = std::make_unique<ParticleCharacterModel>();
        std::string error;
        if (!LoadParticleCharacterModel(settings.model, settings.count, 1, settings.clip, *model, error)) {
            return "ERR " + error;
        }
        m_model = std::move(model);
        m_view = settings;

        // Model-space bounds of the first frame fix the world transform: feet on at[1], centred on at[0], at[2].
        m_scale = 1.0f;
        m_offset[0] = m_offset[1] = m_offset[2] = 0.0f;
        if (!EvaluateWorldTargets(0.0f)) {
            m_model.reset();
            return "ERR pose evaluation failed";
        }
        float bmin[3] = { (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)() };
        float bmax[3] = { -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)(), -(std::numeric_limits<float>::max)() };
        for (size_t i = 0; i < m_targets.size(); i += 3) {
            for (int a = 0; a < 3; ++a) {
                bmin[a] = (std::min)(bmin[a], m_targets[i + a]);
                bmax[a] = (std::max)(bmax[a], m_targets[i + a]);
            }
        }
        const float modelHeight = bmax[1] - bmin[1];
        if (!(modelHeight > 0.0f)) {
            m_model.reset();
            return "ERR model has no height";
        }
        m_scale = settings.height / modelHeight;
        m_offset[0] = settings.at[0] - m_scale * 0.5f * (bmin[0] + bmax[0]);
        m_offset[1] = settings.at[1] - m_scale * bmin[1];
        m_offset[2] = settings.at[2] - m_scale * 0.5f * (bmin[2] + bmax[2]);
        EvaluateWorldTargets(0.0f);

        // Same parameterisation as pchar.sim, in world units of the scaled model.
        const float h = settings.height;
        m_settings = Render::ParticleCharacter::MolecularBodySettings{};
        m_settings.force = settings.force;
        m_settings.scatterSpeed = 1.5f * h;
        m_settings.gravity = 9.8f * h / 1.6f;
        m_settings.swirlRise = 0.5f * h;
        m_settings.floorY = settings.at[1];
        m_settings.centre[0] = settings.at[0];
        m_settings.centre[1] = settings.at[1];
        m_settings.centre[2] = settings.at[2];

        m_body.Reset(m_targets.data(), static_cast<uint32_t>(m_targets.size() / 3), 1);
        m_time = 0.0f;
        m_sinceCycle = 0.0f;

        std::ostringstream out;
        out << std::fixed << std::setprecision(3);
        out << "OK model=" << settings.model << " particles=" << m_body.Count() << " scale=" << m_scale
            << " sampleMs=" << m_model->sampleMs << " bakeMs=" << m_model->bakeMs
            << " clipDuration=" << m_model->clipDuration;
        return out.str();
    }

    std::string ParticleCharacterOverlay::Stop()
    {
        const bool wasRunning = IsRunning();
        m_model.reset();
        return wasRunning ? "OK stopped" : "OK not running";
    }

    std::string ParticleCharacterOverlay::Status() const
    {
        if (!IsRunning()) {
            return "OK running=0";
        }
        double bondSum = 0.0;
        for (float b : m_body.Bonds()) {
            bondSum += b;
        }
        const double meanBond = m_body.Count() > 0 ? bondSum / static_cast<double>(m_body.Count()) : 0.0;
        std::ostringstream out;
        out << std::fixed << std::setprecision(3);
        out << "OK running=1 model=" << m_view.model << " particles=" << m_body.Count()
            << " mode=" << MolecularForceName(m_view.force) << " goal=" << m_body.BondGoal()
            << " meanBond=" << meanBond << " cycle=" << m_view.cycle << " time=" << m_time
            << " maxTargetDist=" << m_body.MaxTargetDistance(m_targets.data())
            << " stepMs=" << m_lastStepMs << " drawMs=" << m_lastDrawMs << " drawn=" << m_lastDrawn;
        return out.str();
    }

    void ParticleCharacterOverlay::SetForce(Render::ParticleCharacter::MolecularForce force)
    {
        m_view.force = force;
        m_settings.force = force;
    }

    void ParticleCharacterOverlay::SetBondGoal(float goal)
    {
        if (IsRunning()) {
            m_body.SetBondGoal(goal);
            m_sinceCycle = 0.0f;
        }
    }

    bool ParticleCharacterOverlay::EvaluateWorldTargets(float t)
    {
        if (!EvaluateParticleCharacterPose(*m_model, t, m_poseP, m_poseN)) {
            return false;
        }
        const size_t n = m_model->set.samples.size();
        m_targets.resize(n * 3);
        m_normals.resize(n * 3);
        Render::ParticleCharacter::InterpolateSamples(m_model->set, m_poseP.data(), m_poseN.data(), 3,
                                                      m_targets.data(), m_normals.data());
        for (size_t i = 0; i < n * 3; i += 3) {
            for (int a = 0; a < 3; ++a) {
                m_targets[i + a] = m_targets[i + a] * m_scale + m_offset[a];
            }
        }
        return true;
    }

    void ParticleCharacterOverlay::Tick(float deltaTime)
    {
        if (!IsRunning() || !(deltaTime > 0.0f)) {
            return;
        }
        const float dt = (std::min)(deltaTime, MAX_STEP_DT);
        const auto start = std::chrono::steady_clock::now();
        if (m_view.cycle > 0.0f) {
            m_sinceCycle += dt;
            if (m_sinceCycle >= m_view.cycle) {
                m_body.SetBondGoal(m_body.BondGoal() < 0.5f ? 1.0f : 0.0f);
                m_sinceCycle = 0.0f;
            }
        }
        m_time += dt;
        if (m_model->clipDuration > 0.0f) {
            m_time = std::fmod(m_time, m_model->clipDuration);
        }
        if (EvaluateWorldTargets(m_time)) {
            m_body.Step(m_targets.data(), m_normals.data(), dt, m_settings, true);
        }
        m_lastStepMs = MillisecondsSince(start);
    }

    void ParticleCharacterOverlay::Draw(ApplicationCore& app)
    {
        if (!IsRunning()) {
            return;
        }
        Camera* camera = app.GetMainCamera();
        if (camera == nullptr) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        const std::vector<float>& positions = m_body.Positions();
        const std::vector<float>& bonds = m_body.Bonds();
        uint32_t drawn = 0;
        for (uint32_t i = 0; i < m_body.Count(); ++i) {
            ImVec2 screen;
            float depth = 0.0f;
            if (!ProjectToOverlay(proxy, viewport->Pos, viewport->Size, &positions[static_cast<size_t>(i) * 3], screen, &depth)) {
                continue;
            }
            const float pixels = (std::min)((std::max)(POINT_WORLD_SIZE * OverlayPixelsPerUnit(proxy, viewport->Size, depth),
                                                       MIN_POINT_PIXELS), MAX_POINT_PIXELS);
            const float half = 0.5f * pixels;
            drawList->AddRectFilled(ImVec2(screen.x - half, screen.y - half), ImVec2(screen.x + half, screen.y + half),
                                    BondColor(bonds[i]));
            ++drawn;
        }
        m_lastDrawn = drawn;
        m_lastDrawMs = MillisecondsSince(start);
    }
}
