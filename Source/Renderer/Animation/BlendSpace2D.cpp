#include "Renderer/Animation/BlendSpace2D.h"

#include "Renderer/Animation/ClipSampler.h"
#include "Renderer/Animation/PoseBlend.h"

#include <algorithm>
#include <cmath>
#include <vector>

// 2D blend space: gradient band interpolation (Johansen 2009, sec. 6.3), the weighting behind Unity's
// 2D Freeform Cartesian / Directional blend trees. All clips are sampled at the same normalized phase.
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        constexpr float MIN_LENGTH = 1e-6f;
        constexpr float MIN_BAND_DEN = 1e-12f;
        constexpr float MIN_WEIGHT_SUM = 1e-12f;

        float Clamp01(float value)
        {
            return (std::max)(0.0f, (std::min)(1.0f, value));
        }

        float SignedAngle(float ax, float ay, float bx, float by)
        {
            const float lengthA = std::sqrt(ax * ax + ay * ay);
            const float lengthB = std::sqrt(bx * bx + by * by);
            if (lengthA < MIN_LENGTH || lengthB < MIN_LENGTH)
                return 0.0f;
            return std::atan2(ax * by - ay * bx, ax * bx + ay * by);
        }
    }

    void BlendSpace2D::AddSample(const SkeletonAnimation* clip, float x, float y)
    {
        if (!clip)
            return;

        BlendSpace2DSample sample;
        sample.clip = clip;
        sample.x = x;
        sample.y = y;
        m_samples.push_back(sample);
    }

    void BlendSpace2D::ComputeWeights(float x, float y, std::vector<float>& outWeights) const
    {
        const size_t count = m_samples.size();
        outWeights.assign(count, 0.0f);
        if (count == 0)
            return;
        if (count == 1)
        {
            outWeights[0] = 1.0f;
            return;
        }

        const bool directional = m_mode == BlendSpace2DMode::FreeformDirectional;
        const float magnitudeP = std::sqrt(x * x + y * y);

        float sum = 0.0f;
        for (size_t i = 0; i < count; ++i)
        {
            const BlendSpace2DSample& si = m_samples[i];
            const float magnitudeI = std::sqrt(si.x * si.x + si.y * si.y);

            float h = 1.0f;
            for (size_t j = 0; j < count; ++j)
            {
                if (j == i)
                    continue;

                const BlendSpace2DSample& sj = m_samples[j];
                float vijX = sj.x - si.x;
                float vijY = sj.y - si.y;
                float vipX = x - si.x;
                float vipY = y - si.y;

                if (directional)
                {
                    const float magnitudeJ = std::sqrt(sj.x * sj.x + sj.y * sj.y);
                    const float magnitudeSum = magnitudeI + magnitudeJ;
                    if (magnitudeSum >= MIN_LENGTH)
                    {
                        const float averageMagnitude = magnitudeSum * 0.5f;
                        vijX = (magnitudeJ - magnitudeI) / averageMagnitude;
                        vijY = SignedAngle(si.x, si.y, sj.x, sj.y) * BlendSpace2D::DIRECTIONAL_ANGLE_WEIGHT;
                        vipX = (magnitudeP - magnitudeI) / averageMagnitude;
                        vipY = SignedAngle(si.x, si.y, x, y) * BlendSpace2D::DIRECTIONAL_ANGLE_WEIGHT;
                    }
                }

                const float den = vijX * vijX + vijY * vijY;
                if (den < MIN_BAND_DEN)
                    continue;

                const float num = vipX * vijX + vipY * vijY;
                h = (std::min)(h, Clamp01(1.0f - num / den));
            }

            outWeights[i] = h;
            sum += h;
        }

        if (sum <= MIN_WEIGHT_SUM)
        {
            size_t nearest = 0;
            float nearestDistance = 0.0f;
            for (size_t i = 0; i < count; ++i)
            {
                const float dx = m_samples[i].x - x;
                const float dy = m_samples[i].y - y;
                const float distance = dx * dx + dy * dy;
                if (i == 0 || distance < nearestDistance)
                {
                    nearest = i;
                    nearestDistance = distance;
                }
            }
            outWeights.assign(count, 0.0f);
            outWeights[nearest] = 1.0f;
            return;
        }

        const float invSum = 1.0f / sum;
        for (float& w : outWeights)
            w *= invSum;
    }

    float BlendSpace2D::BlendedDuration(float x, float y) const
    {
        std::vector<float> weights;
        ComputeWeights(x, y, weights);
        float duration = 0.0f;
        for (size_t i = 0; i < weights.size(); ++i)
            duration += weights[i] * m_samples[i].clip->durationSec;
        return duration;
    }

    void BlendSpace2D::Evaluate(float x, float y, float phase, const LocalPose& bindPose, LocalPose& out) const
    {
        out = bindPose;

        std::vector<float>& weights = m_weights;
        LocalPose& scratch = m_scratch;
        ComputeWeights(x, y, weights);

        float accumulated = 0.0f;
        for (size_t i = 0; i < weights.size(); ++i)
        {
            const float w = weights[i];
            if (w <= 0.0f)
                continue;

            const SkeletonAnimation& clip = *m_samples[i].clip;
            const float time = WrapClipTime(phase * clip.durationSec, clip.durationSec, true);
            if (accumulated <= 0.0f)
            {
                out = bindPose;
                SampleClip(clip, time, out);
                accumulated = w;
            }
            else
            {
                scratch = bindPose;
                SampleClip(clip, time, scratch);
                BlendPoses(out, scratch, w / (accumulated + w), out);
                accumulated += w;
            }
        }
    }

    RootMotionDelta BlendSpace2D::EvaluateRootMotion(float x, float y, float phase0, float phase1,
                                                     const Skeleton& skeleton, int rootBone) const
    {
        RootMotionDelta result;
        std::vector<float>& weights = m_weights;
        ComputeWeights(x, y, weights);

        for (size_t i = 0; i < weights.size(); ++i)
        {
            const float w = weights[i];
            if (w <= 0.0f)
                continue;

            const SkeletonAnimation& clip = *m_samples[i].clip;
            const float d = clip.durationSec;
            const RootMotionDelta delta = ExtractRootMotion(clip, skeleton, rootBone, phase0 * d, phase1 * d, true);
            result.translation = result.translation + delta.translation * w;
            result.yaw += delta.yaw * w;
        }
        return result;
    }
}
