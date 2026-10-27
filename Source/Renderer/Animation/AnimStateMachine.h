#pragma once
#include "Renderer/Animation/AnimNotifyTrack.h"
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Animation/BlendSpace1D.h"
#include "Renderer/Animation/BlendSpace2D.h"
#include "Renderer/Animation/Inertialization.h"
#include "Renderer/Animation/RootMotion.h"
#include "Renderer/Structures/Skeleton.h"
#include "Renderer/Structures/SkeletonAnimation.h"

#include <string>
#include <unordered_map>
#include <vector>

// Animation state machine (UE "Anim State Machine", Unity Mecanim Animator Controller).
// States play a clip or a 1D blend space; transitions fire on parameter conditions and an optional
// exit time. How a transition blends is switchable:
//   Crossfade        - both states are evaluated and lerped over `duration` (classic).
//   Inertialization  - switch at once, hide the pop with decaying offsets (Bollo, GDC 2018);
//                      `duration` is then used as 2x the spring halflife.
namespace SasamiEngine::Render::Anim
{
    enum class TransitionBlendMode
    {
        Crossfade,
        Inertialization,
    };

    enum class ConditionOp
    {
        Greater,    // float param > threshold
        Less,       // float param < threshold
        IsTrue,     // bool param
        IsFalse,
        Trigger,    // trigger param set; consumed when the transition fires
    };

    struct AnimCondition
    {
        std::string param;
        ConditionOp op = ConditionOp::IsTrue;
        float threshold = 0.0f;
    };

    struct AnimState
    {
        std::string name;
        const SkeletonAnimation* clip = nullptr;    // either a clip ...
        const BlendSpace1D* blendSpace = nullptr;   // ... or a blend space (wins when both set)
        const BlendSpace2D* blendSpace2D = nullptr; // ... or a 2D blend space (wins over both)
        std::string blendParam;                     // float parameter that drives the blend space (x for 2D)
        std::string blendParamY;                    // y parameter of the 2D blend space
        // Optional notifies. Times are seconds within one cycle of StateDuration() (the clip length,
        // or the blended length of a blend space), so they follow the playhead as the blend changes.
        const AnimNotifyTrack* notifies = nullptr;
        bool loop = true;
        float speed = 1.0f;
    };

    // One notify event fired by a state during Update (UE AnimNotify / Unity Animation Event).
    struct AnimStateNotifyEvent
    {
        int state = -1;
        AnimNotifyEvent event{};
    };

    struct AnimTransition
    {
        int from = -1;              // -1 = from any state (never to itself)
        int to = 0;
        float duration = 0.2f;      // seconds
        float exitTime = -1.0f;     // normalized time of `from` the transition waits for; < 0 = any time
        std::vector<AnimCondition> conditions;  // all must hold (empty = only exitTime)
    };

    class AnimStateMachine
    {
    public:
        // `skeleton` must outlive the machine.
        void Initialize(const Skeleton* skeleton);

        int AddState(const AnimState& state);
        void AddTransition(const AnimTransition& transition);
        int FindState(const std::string& name) const;
        const AnimState& State(int index) const { return m_states[index]; }
        int StateCount() const { return static_cast<int>(m_states.size()); }

        void SetFloat(const std::string& name, float value);
        void SetBool(const std::string& name, bool value);
        void SetTrigger(const std::string& name);
        float GetFloat(const std::string& name) const;
        bool GetBool(const std::string& name) const;

        void SetBlendMode(TransitionBlendMode mode) { m_blendMode = mode; }
        TransitionBlendMode BlendMode() const { return m_blendMode; }

        // When enabled, root-bone travel is stripped from the output pose and accumulated for
        // ConsumeRootMotion; otherwise the pose keeps the travel and the delta stays zero.
        void SetRootMotionEnabled(bool enabled) { m_rootMotion = enabled; }
        bool RootMotionEnabled() const { return m_rootMotion; }
        int RootBone() const { return m_rootBone; }

        // Jumps to `state` with no blend (resets inertialization).
        void Start(int state);
        // Forces a transition now, ignoring conditions.
        void ForceTransition(int to, float duration);

        // Advances time, evaluates transitions (at most one per update), writes the pose.
        void Update(float dt, LocalPose& outPose);

        int CurrentState() const { return m_current.state; }
        int PreviousState() const { return m_previous.state; }
        bool InTransition() const;
        float TransitionAlpha() const;      // 0..1 progress of the active transition, 1 when none
        float CurrentNormalizedTime() const { return m_current.phase; }
        float CurrentTime() const;          // seconds into the current state's cycle

        // Root motion accumulated since the last call, then cleared.
        RootMotionDelta ConsumeRootMotion();
        // Notify events fired since the last call, in firing order. Only the current (destination)
        // state fires; a state being faded out is silent (UE fires both by weight; kept simple here).
        // Entering a state fires notifies placed exactly at time 0.
        std::vector<AnimStateNotifyEvent> ConsumeNotifyEvents();

    private:
        struct StateInstance
        {
            int state = -1;
            float phase = 0.0f;     // normalized; may exceed 1 for one-shot clips (clamped when sampling)
            float elapsed = 0.0f;   // seconds since the state was entered
        };

        bool ConditionsHold(const AnimTransition& transition) const;
        const AnimTransition* FindFiringTransition() const;
        void BeginTransition(const AnimTransition& transition, float dt);
        float StateDuration(int state) const;
        void AdvanceInstance(StateInstance& instance, float dt, RootMotionDelta* outMotion);
        void EvaluateInstance(const StateInstance& instance, LocalPose& out) const;

        const Skeleton* m_skeleton = nullptr;
        std::vector<AnimState> m_states;
        std::vector<AnimTransition> m_transitions;
        std::unordered_map<std::string, float> m_floats;
        std::unordered_map<std::string, bool> m_bools;
        std::unordered_map<std::string, bool> m_triggers;

        TransitionBlendMode m_blendMode = TransitionBlendMode::Crossfade;
        bool m_rootMotion = true;
        int m_rootBone = -1;
        Transform m_rootReference{};

        StateInstance m_current{};
        StateInstance m_previous{};
        float m_transitionTime = 0.0f;
        float m_transitionDuration = 0.0f;
        bool m_transitionActive = false;

        PoseInertializer m_inertializer;
        float m_inertializeHalflife = 0.1f;

        LocalPose m_bindPose;
        LocalPose m_scratchA;
        LocalPose m_scratchB;
        LocalPose m_lastOutput;
        LocalPose m_prevOutput;
        float m_lastDt = 0.0f;
        bool m_hasLastOutput = false;
        RootMotionDelta m_pendingMotion{};
        std::vector<AnimStateNotifyEvent> m_pendingEvents;
        std::vector<AnimNotifyEvent> m_eventScratch;
    };
}

namespace SasamiEngine { using namespace Render; }
