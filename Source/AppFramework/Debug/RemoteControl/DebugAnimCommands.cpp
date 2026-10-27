#include "AppFramework/Debug/RemoteControl/DebugAnimCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/ApplicationResourcePaths.h"
#include "AppFramework/Gameplay/GameplaySelfTest.h"
#include "AppFramework/Loader/ModelLoader.h"
#include "Renderer/Animation/AnimSelfTest.h"
#include "Renderer/Animation/AnimStateMachine.h"
#include "Renderer/Animation/BlendSpace1D.h"
#include "Renderer/Animation/BoneMask.h"
#include "Renderer/Animation/CharacterAnimator.h"
#include "Renderer/Animation/ClipSampler.h"
#include "Renderer/Animation/PoseModelSpace.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        const Render::SkeletonAnimation* FindClipContaining(const SkinnedModelData& model, const char* fragment)
        {
            const std::string needle = ToLower(fragment);
            for (const Render::SkeletonAnimation& clip : model.animations) {
                if (ToLower(clip.name).find(needle) != std::string::npos) {
                    return &clip;
                }
            }
            return nullptr;
        }

        // 0 for the first quarter, ramp to maxSpeed over the second, hold, then 0 for the last quarter.
        float ScriptedSpeed(float time, float duration, float maxSpeed)
        {
            const float u = time / duration;
            if (u < 0.25f) {
                return 0.0f;
            }
            if (u < 0.5f) {
                return maxSpeed * (u - 0.25f) / 0.25f;
            }
            if (u < 0.75f) {
                return maxSpeed;
            }
            return 0.0f;
        }

        struct FoxRunResult
        {
            int switches = 0;
            float maxJumpNearSwitch = 0.0f;
            float maxJumpElsewhere = 0.0f;
            float rootX = 0.0f;
            float rootZ = 0.0f;
            float rootYaw = 0.0f;
            Render::Anim::Vec3 finalPosition{};
            float finalYaw = 0.0f;
        };

        FoxRunResult RunFoxScenario(const Render::Skeleton& skeleton, const Render::SkeletonAnimation& survey,
                                    const Render::Anim::BlendSpace1D& blendSpace, Render::Anim::TransitionBlendMode mode,
                                    float seconds)
        {
            using namespace Render::Anim;

            AnimStateMachine machine;
            machine.Initialize(&skeleton);
            machine.SetBlendMode(mode);
            machine.SetRootMotionEnabled(true);

            AnimState idle;
            idle.name = "Idle";
            idle.clip = &survey;
            AnimState locomotion;
            locomotion.name = "Locomotion";
            locomotion.blendSpace = &blendSpace;
            locomotion.blendParam = "speed";
            const int idleIndex = machine.AddState(idle);
            const int locomotionIndex = machine.AddState(locomotion);

            AnimTransition toLoco;
            toLoco.from = idleIndex;
            toLoco.to = locomotionIndex;
            toLoco.duration = 0.25f;
            toLoco.conditions.push_back({ "speed", ConditionOp::Greater, 0.1f });
            machine.AddTransition(toLoco);

            AnimTransition toIdle;
            toIdle.from = locomotionIndex;
            toIdle.to = idleIndex;
            toIdle.duration = 0.25f;
            toIdle.conditions.push_back({ "speed", ConditionOp::Less, 0.1f });
            machine.AddTransition(toIdle);

            machine.SetFloat("speed", 0.0f);
            machine.Start(idleIndex);

            constexpr float DT = 1.0f / 60.0f;
            const int frames = (std::max)(1, static_cast<int>(std::lround(seconds / DT)));

            std::vector<float> jumps(static_cast<size_t>(frames), 0.0f);
            std::vector<int> switchFrames;

            FoxRunResult result;
            LocalPose pose = MakeBindPose(skeleton);
            ModelPose model;
            ModelPose previous;
            Vec3 position{};
            float yaw = 0.0f;
            int lastState = machine.CurrentState();

            for (int frame = 0; frame < frames; ++frame) {
                machine.SetFloat("speed", ScriptedSpeed(static_cast<float>(frame) * DT, seconds, 3.0f));
                machine.Update(DT, pose);

                const RootMotionDelta delta = machine.ConsumeRootMotion();
                result.rootX += delta.translation.x;
                result.rootZ += delta.translation.z;
                result.rootYaw += delta.yaw;
                ApplyRootMotion(position, yaw, delta);

                if (machine.CurrentState() != lastState) {
                    ++result.switches;
                    switchFrames.push_back(frame);
                    lastState = machine.CurrentState();
                }

                LocalToModel(skeleton, pose, model);
                if (frame > 0) {
                    float maxJump = 0.0f;
                    for (size_t i = 0; i < model.size() && i < previous.size(); ++i) {
                        maxJump = (std::max)(maxJump, Length(Mat4Translation(model[i]) - Mat4Translation(previous[i])));
                    }
                    jumps[static_cast<size_t>(frame)] = maxJump;
                }
                previous = model;
            }

            for (int frame = 1; frame < frames; ++frame) {
                bool nearSwitch = false;
                for (const int switchFrame : switchFrames) {
                    if (std::abs(frame - switchFrame) <= 5) {
                        nearSwitch = true;
                        break;
                    }
                }
                float& bucket = nearSwitch ? result.maxJumpNearSwitch : result.maxJumpElsewhere;
                bucket = (std::max)(bucket, jumps[static_cast<size_t>(frame)]);
            }

            result.finalPosition = position;
            result.finalYaw = yaw;
            return result;
        }

        void AppendFoxResult(std::ostringstream& out, const char* label, const FoxRunResult& r)
        {
            out << label << ": switches=" << r.switches
                << " maxJumpNearSwitch=" << r.maxJumpNearSwitch
                << " maxJumpElsewhere=" << r.maxJumpElsewhere
                << " rootMotion=(x=" << r.rootX << " z=" << r.rootZ << " yaw=" << r.rootYaw << ")"
                << " finalPos=(" << r.finalPosition.x << ", " << r.finalPosition.y << ", " << r.finalPosition.z << ")"
                << " finalYaw=" << r.finalYaw << "\n";
        }

        std::string RunFoxCommand(const std::vector<std::string>& args)
        {
            if (args.size() > 2) {
                return "ERR usage: anim.fox [seconds=4] [crossfade|inertial|both=both]";
            }

            float seconds = 4.0f;
            std::string modeText = "both";
            for (const std::string& arg : args) {
                if (const std::optional<float> parsed = ParsePositiveFloat(arg)) {
                    seconds = *parsed;
                } else {
                    modeText = ToLower(arg);
                }
            }
            if (modeText != "crossfade" && modeText != "inertial" && modeText != "both") {
                return "ERR usage: anim.fox [seconds=4] [crossfade|inertial|both=both]";
            }

            const std::string path = ApplicationResourcePaths::ResolveAssetPathString("Models/Fox/Fox.gltf");
            SkinnedModelData model;
            if (!LoadGLTFSkinned(path, model)) {
                return "ERR load failed " + path;
            }
            if (model.skeleton.boneCount == 0 || model.animations.empty()) {
                return "ERR Fox has no skeleton or animations";
            }

            const Render::SkeletonAnimation* survey = FindClipContaining(model, "survey");
            const Render::SkeletonAnimation* walk = FindClipContaining(model, "walk");
            const Render::SkeletonAnimation* run = FindClipContaining(model, "run");
            if (survey == nullptr || walk == nullptr || run == nullptr) {
                std::ostringstream err;
                err << "ERR missing clip(s) survey=" << (survey != nullptr) << " walk=" << (walk != nullptr)
                    << " run=" << (run != nullptr) << "; available:";
                for (const Render::SkeletonAnimation& clip : model.animations) {
                    err << ' ' << clip.name;
                }
                return err.str();
            }

            Render::Anim::BlendSpace1D blendSpace;
            blendSpace.AddSample(walk, 1.0f);
            blendSpace.AddSample(run, 3.0f);

            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "anim.fox seconds=" << seconds << " bones=" << model.skeleton.boneCount
                << " clips=" << model.animations.size() << "\n";

            if (modeText == "crossfade" || modeText == "both") {
                AppendFoxResult(out, "crossfade",
                    RunFoxScenario(model.skeleton, *survey, blendSpace, Render::Anim::TransitionBlendMode::Crossfade, seconds));
            }
            if (modeText == "inertial" || modeText == "both") {
                AppendFoxResult(out, "inertial",
                    RunFoxScenario(model.skeleton, *survey, blendSpace, Render::Anim::TransitionBlendMode::Inertialization, seconds));
            }

            Render::Anim::CharacterAnimatorSettings settings;
            const bool humanoid = Render::Anim::CharacterAnimator::AutoConfigureHumanoid(model.skeleton, settings);
            out << "humanoid auto-configure: " << (humanoid ? "found" : "nothing found (expected for a quadruped)")
                << " lookAtJoints=" << settings.lookAt.joints.size() << " legs=" << settings.legs.size() << "\n";

            Render::Anim::CharacterAnimator animator;
            animator.Initialize(&model.skeleton);
            animator.Settings() = settings;
            animator.Settings().lookAtEnabled = !settings.lookAt.joints.empty();
            animator.Settings().footIkEnabled = !settings.legs.empty();

            Render::Anim::AnimStateMachine& machine = animator.StateMachine();
            Render::Anim::AnimState idle;
            idle.name = "Idle";
            idle.clip = survey;
            machine.AddState(idle);
            machine.Start(0);

            constexpr float DT = 1.0f / 60.0f;
            for (int i = 0; i < 60; ++i) {
                animator.Update(DT);
            }
            out << "character animator: ran 60 frames, pose bones=" << animator.Pose().size()
                << " footIkReached=" << animator.FootIk().ReachedCount() << "\n";
            return out.str();
        }
    }

    void RegisterAnimCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        (void)app;

        registry.Register("anim.selftest", "CPU animation runtime self-test (math, sampling, blend, springs, IK, state machine)",
            [](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) { return "ERR usage: anim.selftest"; }
                return Render::Anim::RunAnimSelfTest();
            });

        registry.Register("gameplay.selftest", "CharacterMovement / FollowCamera / CameraShake self-test",
            [](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) { return "ERR usage: gameplay.selftest"; }
                return Gameplay::RunGameplaySelfTest();
            });

        registry.Register("anim.fox", "anim.fox [seconds=4] [crossfade|inertial|both=both]  Fox locomotion state machine transition-quality report",
            [](const std::vector<std::string>& args) -> std::string {
                return RunFoxCommand(args);
            });
    }
}
