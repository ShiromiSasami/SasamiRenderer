#include "Renderer/Animation/BlendSpace1D.h"

#include "Renderer/Animation/ClipSampler.h"
#include "Renderer/Animation/PoseBlend.h"

#include <algorithm>

// 1D blend space (UE "Blend Space 1D", Unity "1D Blend Tree"): two neighbours blended linearly,
// all clips sampled at the same normalized phase (UE "Sync Groups").
namespace SasamiEngine::Render::Anim
{
    void BlendSpace1D::AddSample(const SkeletonAnimation* clip, float position)
    {
        if (!clip)
            return;

        BlendSpaceSample sample;
        sample.clip = clip;
        sample.position = position;
        const auto it = std::upper_bound(m_samples.begin(), m_samples.end(), position,
            [](float value, const BlendSpaceSample& s) { return value < s.position; });
        m_samples.insert(it, sample);
    }

    void BlendSpace1D::ComputeWeights(float parameter, std::vector<float>& outWeights) const
    {
        const size_t count = m_samples.size();
        outWeights.assign(count, 0.0f);
        if (count == 0)
            return;

        if (count == 1 || parameter <= m_samples.front().position)
        {
            outWeights[0] = 1.0f;
            return;
        }
        if (parameter >= m_samples.back().position)
        {
            outWeights[count - 1] = 1.0f;
            return;
        }

        size_t k = 0;
        while (k + 2 < count && m_samples[k + 1].position <= parameter)
            ++k;

        const float span = m_samples[k + 1].position - m_samples[k].position;
        const float t = span > 0.0f ? (parameter - m_samples[k].position) / span : 0.0f;
        outWeights[k] = 1.0f - t;
        outWeights[k + 1] = t;
    }

    float BlendSpace1D::BlendedDuration(float parameter) const
    {
        std::vector<float> weights;
        ComputeWeights(parameter, weights);
        float duration = 0.0f;
        for (size_t i = 0; i < weights.size(); ++i)
            duration += weights[i] * m_samples[i].clip->durationSec;
        return duration;
    }

    void BlendSpace1D::Evaluate(float parameter, float phase, const LocalPose& bindPose, LocalPose& out) const
    {
        out = bindPose;

        std::vector<float>& weights = m_weights;
        LocalPose& scratch = m_scratch;
        ComputeWeights(parameter, weights);

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

    RootMotionDelta BlendSpace1D::EvaluateRootMotion(float parameter, float phase0, float phase1,
                                                     const Skeleton& skeleton, int rootBone) const
    {
        RootMotionDelta result;
        std::vector<float>& weights = m_weights;
        ComputeWeights(parameter, weights);

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
