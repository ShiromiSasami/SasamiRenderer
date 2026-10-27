#pragma once

#include "AppFramework/Debug/ParticleCharacterModel.h"
#include "Renderer/ParticleCharacter/MolecularBody.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        struct ParticleCharacterViewSettings
        {
            std::string model = "fox";
            uint32_t count = 8000;
            uint32_t clip = 0;
            float height = 1.5f;                       // world height the model is scaled to
            float at[3] = { 0.0f, 0.0f, 0.0f };        // feet centre in world space (floor = at[1])
            Render::ParticleCharacter::MolecularForce force = Render::ParticleCharacter::MolecularForce::Scatter;
            float cycle = 0.0f;                        // > 0: release / re-form every `cycle` seconds
        };

        // pchar.view (particle character stage 1b): plays a baked VAT clip, steps a MolecularBody on its
        // surface samples (scaled into world space) and draws every particle as a small ImGui background
        // square, warm when bonded and cool when free. CPU only and main thread only. There is no depth
        // test: the points draw over the whole frame, scene geometry included.
        class ParticleCharacterOverlay
        {
        public:
            ParticleCharacterOverlay();
            ~ParticleCharacterOverlay();

            // Returns "OK ..." or "ERR ...". Loading blocks (sample + bake, a few hundred ms on Fox).
            std::string Start(const ParticleCharacterViewSettings& settings);
            std::string Stop();
            std::string Status() const;
            bool IsRunning() const { return m_model != nullptr; }

            void SetForce(Render::ParticleCharacter::MolecularForce force);
            Render::ParticleCharacter::MolecularForce Force() const { return m_view.force; }
            void SetBondGoal(float goal);
            float BondGoal() const { return m_body.BondGoal(); }
            void SetCycle(float seconds) { m_view.cycle = seconds > 0.0f ? seconds : 0.0f; m_sinceCycle = 0.0f; }
            float Cycle() const { return m_view.cycle; }
            const ParticleCharacterViewSettings& View() const { return m_view; }

            // Once per frame: advances the clip and steps the body by dt (capped at 1/20 s).
            void Tick(float deltaTime);
            // ImGui overlay callback.
            void Draw(ApplicationCore& app);

        private:
            bool EvaluateWorldTargets(float t);

            ParticleCharacterViewSettings m_view;
            std::unique_ptr<ParticleCharacterModel> m_model;
            Render::ParticleCharacter::MolecularBody m_body;
            Render::ParticleCharacter::MolecularBodySettings m_settings;
            std::vector<float> m_poseP;
            std::vector<float> m_poseN;
            std::vector<float> m_targets;   // world space, samples * 3
            std::vector<float> m_normals;   // samples * 3
            float m_scale = 1.0f;
            float m_offset[3] = { 0.0f, 0.0f, 0.0f };  // world = model * m_scale + m_offset
            float m_time = 0.0f;
            float m_sinceCycle = 0.0f;
            double m_lastStepMs = 0.0;
            double m_lastDrawMs = 0.0;
            uint32_t m_lastDrawn = 0;
        };

        const char* MolecularForceName(Render::ParticleCharacter::MolecularForce force);
        bool ParseMolecularForce(const std::string& text, Render::ParticleCharacter::MolecularForce& out);
    }
}
namespace SasamiEngine { using namespace App; }
