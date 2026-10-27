#include "Renderer/Animation/AnimNotifyTrack.h"

#include <algorithm>
#include <vector>

// Clip notify track (UE "Anim Notify" / "Anim Notify State", Unity "Animation Events"): notifies sorted by
// clip time; Collect reports what a forward playhead move crossed, splitting a loop wrap into two segments.
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        struct Candidate
        {
            float time = 0.0f;
            uint32_t notifyIndex = 0;
            AnimNotifyEventType type = AnimNotifyEventType::Trigger;
        };

        // End closes before Begin opens before Trigger fires at equal times.
        int TypeOrder(AnimNotifyEventType type)
        {
            switch (type)
            {
            case AnimNotifyEventType::End: return 0;
            case AnimNotifyEventType::Begin: return 1;
            default: return 2;
            }
        }

        // (a, b], or [a, b] when closedStart.
        bool InSegment(float value, float a, float b, bool closedStart)
        {
            const bool afterStart = closedStart ? (value >= a) : (value > a);
            return afterStart && value <= b;
        }

        void CollectSegment(const std::vector<AnimNotify>& notifies, float a, float b, bool closedStart,
                            float duration, bool loop, std::vector<AnimNotifyEvent>& outEvents)
        {
            std::vector<Candidate> candidates;
            for (size_t i = 0; i < notifies.size(); ++i)
            {
                const AnimNotify& notify = notifies[i];
                const uint32_t index = static_cast<uint32_t>(i);
                if (notify.duration <= 0.0f)
                {
                    if (InSegment(notify.time, a, b, closedStart))
                        candidates.push_back({ notify.time, index, AnimNotifyEventType::Trigger });
                    continue;
                }

                float endTime = notify.time + notify.duration;
                if (endTime > duration)
                    endTime = (loop && duration > 0.0f) ? (endTime - duration) : duration;

                if (InSegment(notify.time, a, b, closedStart))
                    candidates.push_back({ notify.time, index, AnimNotifyEventType::Begin });
                if (InSegment(endTime, a, b, closedStart))
                    candidates.push_back({ endTime, index, AnimNotifyEventType::End });
            }

            std::sort(candidates.begin(), candidates.end(), [](const Candidate& l, const Candidate& r)
            {
                if (l.time != r.time)
                    return l.time < r.time;
                const int lo = TypeOrder(l.type);
                const int ro = TypeOrder(r.type);
                if (lo != ro)
                    return lo < ro;
                return l.notifyIndex < r.notifyIndex;
            });

            for (const Candidate& c : candidates)
            {
                AnimNotifyEvent event;
                event.notifyIndex = c.notifyIndex;
                event.type = c.type;
                outEvents.push_back(event);
            }
        }
    }

    void AnimNotifyTrack::Add(const AnimNotify& notify)
    {
        AnimNotify copy = notify;
        copy.duration = (std::max)(copy.duration, 0.0f);
        const auto it = std::upper_bound(m_notifies.begin(), m_notifies.end(), copy.time,
            [](float value, const AnimNotify& n) { return value < n.time; });
        m_notifies.insert(it, copy);
    }

    void AnimNotifyTrack::Collect(float prevTime, float currTime, float duration, bool loop, bool includeStart,
                                  std::vector<AnimNotifyEvent>& outEvents) const
    {
        if (currTime >= prevTime)
        {
            CollectSegment(m_notifies, prevTime, currTime, includeStart, duration, loop, outEvents);
            return;
        }

        if (!loop)
            return;

        CollectSegment(m_notifies, prevTime, duration, includeStart, duration, loop, outEvents);
        CollectSegment(m_notifies, 0.0f, currTime, true, duration, loop, outEvents);
    }

    bool AnimNotifyTrack::IsStateActive(uint32_t notifyIndex, float time) const
    {
        if (notifyIndex >= m_notifies.size())
            return false;

        const AnimNotify& notify = m_notifies[notifyIndex];
        if (notify.duration <= 0.0f)
            return false;

        return time >= notify.time && time < notify.time + notify.duration;
    }
}
