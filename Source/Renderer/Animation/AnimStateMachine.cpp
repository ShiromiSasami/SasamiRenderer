#include "Renderer/Animation/AnimStateMachine.h"

#include "Renderer/Animation/ClipSampler.h"
#include "Renderer/Animation/PoseBlend.h"

#include <algorithm>
#include <cmath>

// Animation state machine after UE "Anim State Machine" and Unity Mecanim. Crossfade evaluates both
// states and lerps; Inertialization switches at once and decays the pose discontinuity
// (Bollo, "Inertialization: High-Performance Animation Transitions in Gears of War", GDC 2018).
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        constexpr float MIN_STATE_DURATION = 1e-4f;
        constexpr float INERTIAL_VELOCITY_STEP = 1.0f / 60.0f;
    }

    void AnimStateMachine::Initialize(const Skeleton* skeleton)
    {
        m_skeleton = skeleton;
        m_bindPose.clear();
        m_rootBone = -1;
        m_rootReference = Transform{};
        if (!m_skeleton)
        {
            m_inertializer.Reset(0);
            return;
        }

        m_bindPose = MakeBindPose(*m_skeleton);
        m_rootBone = FindRootMotionBone(*m_skeleton);
        if (m_rootBone >= 0 && m_rootBone < static_cast<int>(m_bindPose.size()))
            m_rootReference = m_bindPose[m_rootBone];
        else
            m_rootBone = -1;
        m_inertializer.Reset(m_skeleton->boneCount);
    }

    int AnimStateMachine::AddState(const AnimState& state)
    {
        m_states.push_back(state);
        return static_cast<int>(m_states.size()) - 1;
    }

    void AnimStateMachine::AddTransition(const AnimTransition& transition)
    {
        m_transitions.push_back(transition);
    }

    int AnimStateMachine::FindState(const std::string& name) const
    {
        for (size_t i = 0; i < m_states.size(); ++i)
        {
            if (m_states[i].name == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    void AnimStateMachine::SetFloat(const std::string& name, float value)
    {
        m_floats[name] = value;
    }

    void AnimStateMachine::SetBool(const std::string& name, bool value)
    {
        m_bools[name] = value;
    }

    void AnimStateMachine::SetTrigger(const std::string& name)
    {
        m_triggers[name] = true;
    }

    float AnimStateMachine::GetFloat(const std::string& name) const
    {
        const auto it = m_floats.find(name);
        return it != m_floats.end() ? it->second : 0.0f;
    }

    bool AnimStateMachine::GetBool(const std::string& name) const
    {
        const auto it = m_bools.find(name);
        return it != m_bools.end() ? it->second : false;
    }

    float AnimStateMachine::StateDuration(int state) const
    {
        if (state < 0 || state >= static_cast<int>(m_states.size()))
            return 0.0f;
        const AnimState& s = m_states[state];
        if (s.blendSpace2D)
            return s.blendSpace2D->BlendedDuration(GetFloat(s.blendParam), GetFloat(s.blendParamY));
        if (s.blendSpace)
            return s.blendSpace->BlendedDuration(GetFloat(s.blendParam));
        if (s.clip)
            return s.clip->durationSec;
        return 0.0f;
    }

    void AnimStateMachine::Start(int state)
    {
        const bool valid = state >= 0 && state < static_cast<int>(m_states.size());
        m_current = StateInstance{};
        m_current.state = valid ? state : -1;
        m_previous = StateInstance{};
        m_transitionActive = false;
        m_transitionTime = 0.0f;
        m_transitionDuration = 0.0f;
        m_hasLastOutput = false;
        m_pendingEvents.clear();
        m_inertializer.Reset(m_skeleton ? m_skeleton->boneCount : 0);
    }

    void AnimStateMachine::ForceTransition(int to, float duration)
    {
        if (to < 0 || to >= static_cast<int>(m_states.size()))
            return;
        AnimTransition transition;
        transition.from = -1;
        transition.to = to;
        transition.duration = duration;
        BeginTransition(transition, 0.0f);
    }

    void AnimStateMachine::AdvanceInstance(StateInstance& instance, float dt, RootMotionDelta* outMotion)
    {
        if (instance.state < 0 || instance.state >= static_cast<int>(m_states.size()))
            return;

        const AnimState& st = m_states[instance.state];
        const float d = std::max(StateDuration(instance.state), MIN_STATE_DURATION);
        const float dPhase = dt * st.speed / d;
        const float phase0 = instance.phase;
        float phase1 = phase0 + dPhase;
        if (!st.loop)
            phase1 = std::clamp(phase1, 0.0f, 1.0f);

        const bool wantMotion = outMotion && m_rootMotion && m_rootBone >= 0 && m_skeleton && phase1 > phase0;
        if (wantMotion)
        {
            if (st.blendSpace2D)
            {
                *outMotion = st.blendSpace2D->EvaluateRootMotion(GetFloat(st.blendParam), GetFloat(st.blendParamY),
                                                                 phase0, phase1, *m_skeleton, m_rootBone);
            }
            else if (st.blendSpace)
            {
                *outMotion = st.blendSpace->EvaluateRootMotion(GetFloat(st.blendParam), phase0, phase1, *m_skeleton,
                                                               m_rootBone);
            }
            else if (st.clip)
            {
                const float clipDuration = st.clip->durationSec;
                *outMotion = ExtractRootMotion(*st.clip, *m_skeleton, m_rootBone, phase0 * clipDuration,
                                               phase1 * clipDuration, st.loop);
            }
        }

        const bool firstStep = instance.elapsed <= 0.0f;
        instance.phase = st.loop ? phase1 - std::floor(phase1) : phase1;
        instance.elapsed += dt;

        // Notifies: only the current state fires (not the faded-out one, not the inertialization probe).
        if (&instance == &m_current && outMotion && st.notifies && (phase1 > phase0 || firstStep))
        {
            const float prevTime = std::min(phase0, 1.0f) * d;
            const float currTime = std::min(instance.phase, 1.0f) * d;
            m_eventScratch.clear();
            st.notifies->Collect(prevTime, currTime, d, st.loop, firstStep, m_eventScratch);
            for (const AnimNotifyEvent& e : m_eventScratch)
                m_pendingEvents.push_back({ instance.state, e });
        }
    }

    void AnimStateMachine::EvaluateInstance(const StateInstance& instance, LocalPose& out) const
    {
        out = m_bindPose;
        if (instance.state < 0 || instance.state >= static_cast<int>(m_states.size()))
            return;

        const AnimState& st = m_states[instance.state];
        if (st.blendSpace2D)
        {
            st.blendSpace2D->Evaluate(GetFloat(st.blendParam), GetFloat(st.blendParamY), instance.phase, m_bindPose,
                                      out);
        }
        else if (st.blendSpace)
        {
            st.blendSpace->Evaluate(GetFloat(st.blendParam), instance.phase, m_bindPose, out);
        }
        else if (st.clip)
        {
            const float d = st.clip->durationSec;
            const float time = st.loop ? WrapClipTime(instance.phase * d, d, true) : std::min(instance.phase, 1.0f) * d;
            SampleClip(*st.clip, time, out);
        }

        if (m_rootMotion && m_rootBone >= 0 && m_rootBone < static_cast<int>(out.size()))
            StripRootMotion(out, m_rootBone, m_rootReference);
    }

    bool AnimStateMachine::ConditionsHold(const AnimTransition& transition) const
    {
        if (transition.exitTime >= 0.0f)
        {
            const float phase = m_current.phase;
            if (phase < transition.exitTime)
                return false;
        }

        for (const AnimCondition& c : transition.conditions)
        {
            switch (c.op)
            {
            case ConditionOp::Greater:
                if (!(GetFloat(c.param) > c.threshold))
                    return false;
                break;
            case ConditionOp::Less:
                if (!(GetFloat(c.param) < c.threshold))
                    return false;
                break;
            case ConditionOp::IsTrue:
                if (!GetBool(c.param))
                    return false;
                break;
            case ConditionOp::IsFalse:
                if (GetBool(c.param))
                    return false;
                break;
            case ConditionOp::Trigger:
            {
                const auto it = m_triggers.find(c.param);
                if (it == m_triggers.end() || !it->second)
                    return false;
                break;
            }
            }
        }
        return true;
    }

    const AnimTransition* AnimStateMachine::FindFiringTransition() const
    {
        const int stateCount = static_cast<int>(m_states.size());
        for (const AnimTransition& t : m_transitions)
        {
            if (t.to < 0 || t.to >= stateCount)
                continue;
            if (t.from == -1)
            {
                if (t.to == m_current.state)
                    continue;
            }
            else if (t.from != m_current.state)
            {
                continue;
            }
            if (ConditionsHold(t))
                return &t;
        }
        return nullptr;
    }

    void AnimStateMachine::BeginTransition(const AnimTransition& transition, [[maybe_unused]] float dt)
    {
        for (const AnimCondition& c : transition.conditions)
        {
            if (c.op == ConditionOp::Trigger)
                m_triggers[c.param] = false;
        }

        const float duration = std::max(transition.duration, 0.0f);
        m_previous = m_current;
        m_current = StateInstance{};
        m_current.state = transition.to;

        if (m_blendMode == TransitionBlendMode::Crossfade)
        {
            m_transitionActive = duration > 0.0f;
            m_transitionTime = 0.0f;
            m_transitionDuration = duration;
            return;
        }

        m_transitionActive = false;
        m_inertializeHalflife = std::max(duration * 0.5f, 1e-3f);
        if (!m_hasLastOutput)
            return;

        PoseVelocity displayedVel;
        ComputePoseVelocity(m_prevOutput, m_lastOutput, m_lastDt, displayedVel);

        EvaluateInstance(m_current, m_scratchA);
        StateInstance probe = m_current;
        AdvanceInstance(probe, INERTIAL_VELOCITY_STEP, nullptr);
        EvaluateInstance(probe, m_scratchB);
        PoseVelocity targetVel;
        ComputePoseVelocity(m_scratchA, m_scratchB, INERTIAL_VELOCITY_STEP, targetVel);

        m_inertializer.Transition(m_lastOutput, displayedVel, m_scratchA, targetVel);
    }

    void AnimStateMachine::Update(float dt, LocalPose& outPose)
    {
        if (m_current.state < 0 || m_current.state >= static_cast<int>(m_states.size()))
        {
            outPose = m_bindPose;
            return;
        }

        if (const AnimTransition* firing = FindFiringTransition())
            BeginTransition(*firing, dt);

        RootMotionDelta motion;
        AdvanceInstance(m_current, dt, &motion);

        float alpha = 1.0f;
        const bool crossfading = m_transitionActive && m_previous.state >= 0;
        if (crossfading)
        {
            RootMotionDelta previousMotion;
            AdvanceInstance(m_previous, dt, &previousMotion);
            m_transitionTime += dt;
            alpha = m_transitionDuration > 0.0f ? std::clamp(m_transitionTime / m_transitionDuration, 0.0f, 1.0f) : 1.0f;
            motion = BlendRootMotion(previousMotion, motion, alpha);
        }
        else
        {
            m_transitionActive = false;
        }

        m_pendingMotion.translation += QuatRotate(QuatFromYaw(m_pendingMotion.yaw), motion.translation);
        m_pendingMotion.yaw += motion.yaw;

        if (crossfading)
        {
            EvaluateInstance(m_previous, m_scratchA);
            EvaluateInstance(m_current, m_scratchB);
            BlendPoses(m_scratchA, m_scratchB, alpha, outPose);
            if (alpha >= 1.0f)
            {
                m_transitionActive = false;
                m_previous.state = -1;
            }
        }
        else
        {
            EvaluateInstance(m_current, outPose);
        }

        if (m_blendMode == TransitionBlendMode::Inertialization)
            m_inertializer.Update(outPose, m_inertializeHalflife, dt, outPose);

        m_prevOutput = m_hasLastOutput ? m_lastOutput : outPose;
        m_lastOutput = outPose;
        m_lastDt = dt;
        m_hasLastOutput = true;
    }

    bool AnimStateMachine::InTransition() const
    {
        return m_transitionActive || (m_blendMode == TransitionBlendMode::Inertialization && m_inertializer.IsActive());
    }

    float AnimStateMachine::TransitionAlpha() const
    {
        if (!m_transitionActive)
            return 1.0f;
        if (m_transitionDuration <= 0.0f)
            return 1.0f;
        return std::clamp(m_transitionTime / m_transitionDuration, 0.0f, 1.0f);
    }

    float AnimStateMachine::CurrentTime() const
    {
        return m_current.phase * StateDuration(m_current.state);
    }

    RootMotionDelta AnimStateMachine::ConsumeRootMotion()
    {
        const RootMotionDelta result = m_pendingMotion;
        m_pendingMotion = RootMotionDelta{};
        return result;
    }

    std::vector<AnimStateNotifyEvent> AnimStateMachine::ConsumeNotifyEvents()
    {
        std::vector<AnimStateNotifyEvent> result;
        result.swap(m_pendingEvents);
        return result;
    }
}
