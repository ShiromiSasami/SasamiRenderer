#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Structures/Skeleton.h"

#include <functional>
#include <vector>

// Ground-adaptive foot placement: probe the ground below each foot, lower the pelvis so the lowest
// foot can reach, then solve each leg with two-bone IK (UE "Leg IK"/"Foot Placement", Unity
// "Foot IK"; pelvis-drop approach described by Rune Skovbo Johansen's "Locomotion System").
namespace SasamiEngine::Render::Anim
{
    // World-space vertical probe at (x, z). Returns false when there is no ground there.
    using GroundProbe = std::function<bool(float x, float z, float& outHeight, Vec3& outNormal)>;

    struct FootIkLeg
    {
        int hip = -1;                   // thigh / upper leg
        int knee = -1;
        int ankle = -1;
        Vec3 midAxis{};                 // knee-local bend axis; zero = derive with ComputeMidAxis each solve
        float ankleHeight = 0.0f;       // ankle above the sole, model units (measured from the bind pose)
    };

    struct FootIkSettings
    {
        int pelvis = -1;                // bone moved down by the pelvis offset; -1 = no pelvis adjustment
        float maxPelvisDrop = 0.5f;     // model units
        float maxFootRaise = 0.5f;      // a foot is never pushed up by more than this (model units)
        float pelvisHalflife = 0.08f;   // spring halflife for the pelvis offset, seconds
        float footHalflife = 0.05f;     // spring halflife for each foot's vertical offset
        float soften = 0.97f;           // TwoBoneIkInput::soften
        bool alignToNormal = true;      // rotate the ankle so the foot sole follows the ground slope
        float maxAlignAngle = 0.6f;     // radians
        float weight = 1.0f;
    };

    class FootIkSolver
    {
    public:
        // Fills ankleHeight of every leg from the bind pose (ankle model-space y above the lowest ankle,
        // i.e. 0 for a rig whose ankles touch y = 0, otherwise the ankle's bind height).
        static void MeasureLegs(const Skeleton& skeleton, std::vector<FootIkLeg>& legs);

        void Reset();

        // `pose` and `model` (model == LocalToModel(pose) on entry) are corrected in place.
        // `modelToWorld` places the skeleton in the world (includes the character scale).
        void Solve(const Skeleton& skeleton, const Mat4& modelToWorld, const GroundProbe& probe,
                   const std::vector<FootIkLeg>& legs, const FootIkSettings& settings, float dt,
                   LocalPose& pose, ModelPose& model);

        float PelvisOffset() const { return m_pelvisOffset; }   // model units, <= 0
        // Number of legs whose last solve reached the target.
        int ReachedCount() const { return m_reached; }

    private:
        float m_pelvisOffset = 0.0f;
        float m_pelvisVelocity = 0.0f;
        std::vector<float> m_footOffset;
        std::vector<float> m_footVelocity;
        int m_reached = 0;
    };
}

namespace SasamiEngine { using namespace Render; }
