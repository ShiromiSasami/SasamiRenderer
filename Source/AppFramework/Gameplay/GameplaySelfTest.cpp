#include "Gameplay/GameplaySelfTest.h"

#include "Gameplay/CameraShake.h"
#include "Gameplay/CharacterMovement.h"
#include "Gameplay/FollowCamera.h"
#include "Renderer/Animation/AnimMath.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

// References: Holden, "Spring-It-On"; UE USpringArmComponent; Eiserloh, GDC 2016 (camera trauma).
namespace SasamiEngine::App::Gameplay
{
    using namespace Render::Anim;

    namespace
    {
        class Report
        {
        public:
            void Check(const char* name, bool pass, const std::string& detail)
            {
                ++m_total;
                if (pass) {
                    ++m_passed;
                }
                m_out << (pass ? "ok " : "FAIL ") << name << " " << detail << "\n";
            }

            std::string Finish()
            {
                m_out << "gameplay selftest: " << m_passed << "/" << m_total << " passed\n";
                return m_out.str();
            }

        private:
            std::ostringstream m_out;
            int m_passed = 0;
            int m_total = 0;
        };

        std::string Fmt(const char* label, float value)
        {
            std::ostringstream s;
            s << std::fixed << std::setprecision(6) << label << "=" << value;
            return s.str();
        }

        std::string Fmt2(const char* l0, float v0, const char* l1, float v1)
        {
            return Fmt(l0, v0) + " " + Fmt(l1, v1);
        }

        MovementInput ForwardInput(float cameraYaw = 0.0f)
        {
            MovementInput in;
            in.moveY = 1.0f;
            in.cameraYaw = cameraYaw;
            return in;
        }

        void TestMovement(Report& r)
        {
            const float dt = 1.0f / 60.0f;
            {
                CharacterMovement m;
                m.Teleport({ 0.0f, 0.0f, 0.0f }, 0.0f);
                for (int i = 0; i < 180; ++i) {
                    m.Update(dt, ForwardInput());
                }
                const float walk = m.Settings().walkSpeed;
                const Vec3 p = m.Position();
                r.Check("movement.walk_speed", std::fabs(m.HorizontalSpeed() - walk) <= walk * 0.02f,
                        Fmt2("speed", m.HorizontalSpeed(), "expected", walk));
                r.Check("movement.walk_direction", p.z > 1.0f && std::fabs(p.x) < 1e-3f && std::fabs(m.Yaw()) < 0.01f,
                        Fmt2("z", p.z, "yaw", m.Yaw()));

                MovementInput stop;
                for (int i = 0; i < 120; ++i) {
                    m.Update(dt, stop);
                }
                r.Check("movement.stop", m.HorizontalSpeed() < 0.05f, Fmt("speed", m.HorizontalSpeed()));
            }
            {
                CharacterMovement m;
                m.Teleport({ 0.0f, 0.0f, 0.0f }, 0.0f);
                for (int i = 0; i < 180; ++i) {
                    m.Update(dt, ForwardInput(ANIM_PI * 0.5f));
                }
                const Vec3 p = m.Position();
                r.Check("movement.camera_relative", p.x > 1.0f && std::fabs(p.z) < 1e-3f, Fmt2("x", p.x, "z", p.z));
            }
            {
                CharacterMovement a;
                CharacterMovement b;
                a.Teleport({}, 0.0f);
                b.Teleport({}, 0.0f);
                for (int i = 0; i < 60; ++i) {
                    a.Update(1.0f / 30.0f, ForwardInput());
                }
                for (int i = 0; i < 240; ++i) {
                    b.Update(1.0f / 120.0f, ForwardInput());
                }
                const float err = Length(a.Position() - b.Position());
                r.Check("movement.framerate_independence", err < 1e-2f, Fmt("posDiff", err));
            }
        }

        void TestJump(Report& r)
        {
            const float dt = 1.0f / 60.0f;
            CharacterMovement m;
            m.Teleport({ 0.0f, 0.0f, 0.0f }, 0.0f);
            float maxY = 0.0f;
            int jumpEvents = 0;
            bool landed = false;
            for (int i = 0; i < 150; ++i) {
                MovementInput in;
                in.jump = (i == 0 || i == 10 || i == 20);
                m.Update(dt, in);
                maxY = std::max(maxY, m.Position().y);
                jumpEvents += m.JustJumped() ? 1 : 0;
                landed = landed || m.JustLanded();
            }
            const CharacterMovementSettings& s = m.Settings();
            const float expected = s.jumpSpeed * s.jumpSpeed / (2.0f * std::fabs(s.gravity));
            r.Check("jump.apex", std::fabs(maxY - expected) <= expected * 0.05f, Fmt2("apex", maxY, "expected", expected));
            r.Check("jump.landing", landed && m.Grounded() && std::fabs(m.Position().y) < 1e-4f,
                    Fmt("endY", m.Position().y));
            r.Check("jump.no_double_jump", jumpEvents == 1, Fmt("jumpEvents", static_cast<float>(jumpEvents)));
        }

        void TestTrajectory(Report& r)
        {
            const float dt = 1.0f / 60.0f;
            CharacterMovement m;
            m.Teleport({ 1.0f, 0.0f, -2.0f }, 0.0f);
            for (int i = 0; i < 10; ++i) {
                m.Update(dt, ForwardInput(0.4f));
            }
            constexpr int COUNT = 30;
            Vec3 predicted[COUNT];
            m.PredictTrajectory(predicted, COUNT, dt);
            float maxErr = 0.0f;
            for (int i = 0; i < COUNT; ++i) {
                m.Update(dt, ForwardInput(0.4f));
                const Vec3 d = m.Position() - predicted[i];
                maxErr = std::max(maxErr, Length(d));
            }
            r.Check("trajectory.matches_stepping", maxErr < 1e-2f, Fmt("maxErr", maxErr));
        }

        void TestCamera(Report& r)
        {
            const float dt = 1.0f / 60.0f;
            FollowCamera cam;
            cam.SetOrbit(0.5f, 0.3f);
            cam.Snap({ 0.0f, 0.0f, 0.0f });

            const FollowCameraSettings& s = cam.Settings();
            const Quat rot = QuatMul(QuatFromYaw(cam.Yaw()), QuatFromAxisAngle({ 1.0f, 0.0f, 0.0f }, cam.Pitch()));
            const Vec3 right = QuatRotate(rot, { 1.0f, 0.0f, 0.0f });
            const Vec3 shoulderPivot = Vec3{ 0.0f, s.pivotHeight, 0.0f } + right * s.shoulderOffset;
            const float dist = Length(cam.Position() - shoulderPivot);
            r.Check("camera.snap_distance", std::fabs(dist - s.distance) < 1e-4f, Fmt2("dist", dist, "expected", s.distance));
            r.Check("camera.snap_above_target", cam.Position().y > shoulderPivot.y, Fmt("camY", cam.Position().y));

            float fraction = 0.5f;
            cam.SetCollisionQuery([&fraction](const Vec3&, const Vec3&, float) { return fraction; });
            cam.Update(dt, { 0.0f, 0.0f, 0.0f });
            r.Check("camera.collision_instant", std::fabs(cam.ArmLength() - 0.5f * s.distance) < 1e-4f,
                    Fmt2("arm", cam.ArmLength(), "expected", 0.5f * s.distance));

            fraction = 1.0f;
            cam.Update(dt, { 0.0f, 0.0f, 0.0f });
            r.Check("camera.extend_smooth", cam.ArmLength() > 0.5f * s.distance && cam.ArmLength() < 0.99f * s.distance,
                    Fmt("arm", cam.ArmLength()));
            for (int i = 0; i < 120; ++i) {
                cam.Update(dt, { 0.0f, 0.0f, 0.0f });
            }
            r.Check("camera.extend_full", cam.ArmLength() >= 0.99f * s.distance && cam.ArmLength() <= s.distance,
                    Fmt2("arm", cam.ArmLength(), "expected", s.distance));
        }

        void TestShake(Report& r)
        {
            const float dt = 1.0f / 60.0f;
            CameraShake shake;
            shake.AddTrauma(2.0f);
            r.Check("shake.clamp", shake.Trauma() == 1.0f, Fmt("trauma", shake.Trauma()));
            for (int i = 0; i < 60; ++i) {
                shake.Update(dt);
            }
            r.Check("shake.decay_1s", std::fabs(shake.Trauma() - 0.2f) < 0.01f, Fmt("trauma", shake.Trauma()));
            for (int i = 0; i < 30; ++i) {
                shake.Update(dt);
            }
            const Vec3 o = shake.Offset();
            const bool zero = shake.Trauma() == 0.0f && std::fabs(o.x) + std::fabs(o.y) + std::fabs(o.z) == 0.0f
                && shake.YawOffset() == 0.0f && shake.PitchOffset() == 0.0f && shake.RollOffset() == 0.0f;
            r.Check("shake.zero_when_calm", zero, Fmt("trauma", shake.Trauma()));

            float maxAbs = 0.0f;
            for (int i = 0; i < 1000; ++i) {
                maxAbs = std::max(maxAbs, std::fabs(CameraShake::Noise1D(7u, static_cast<float>(i) * 0.037f - 5.0f)));
            }
            r.Check("shake.noise_range", maxAbs <= 1.0f, Fmt("maxAbs", maxAbs));

            float maxStep = 0.0f;
            for (int i = 0; i < 20000; ++i) {
                const float x = static_cast<float>(i) * 0.001f;
                maxStep = std::max(maxStep, std::fabs(CameraShake::Noise1D(3u, x + 0.001f) - CameraShake::Noise1D(3u, x)));
            }
            r.Check("shake.noise_continuous", maxStep < 0.02f, Fmt("maxStep", maxStep));
        }
    }

    std::string RunGameplaySelfTest()
    {
        Report report;
        TestMovement(report);
        TestJump(report);
        TestTrajectory(report);
        TestCamera(report);
        TestShake(report);
        return report.Finish();
    }
}
