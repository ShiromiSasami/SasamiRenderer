#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Clip-time events (UE "Anim Notify" / "Anim Notify State", Unity "Animation Events").
// A notify is a point in clip time (duration 0) or a window [time, time + duration] (a notify state
// with begin/end). Collect reports what a playhead move from t0 to t1 crossed, so a game can play
// footstep sounds, spawn effects or open hit windows exactly once per crossing.
namespace SasamiEngine::Render::Anim
{
    struct AnimNotify
    {
        std::string name;
        float time = 0.0f;      // seconds in clip time, within [0, clipDuration]
        float duration = 0.0f;  // > 0 makes it a state with Begin/End events
    };

    enum class AnimNotifyEventType
    {
        Trigger,    // point notify crossed
        Begin,      // state window entered
        End,        // state window left
    };

    struct AnimNotifyEvent
    {
        uint32_t notifyIndex = 0;   // index into AnimNotifyTrack::Notifies()
        AnimNotifyEventType type = AnimNotifyEventType::Trigger;
    };

    class AnimNotifyTrack
    {
    public:
        // Notifies are kept sorted by time (stable for equal times).
        void Add(const AnimNotify& notify);
        const std::vector<AnimNotify>& Notifies() const { return m_notifies; }
        void Clear() { m_notifies.clear(); }

        // Appends the events crossed when the playhead moves forward from prevTime to currTime
        // (both clip times in [0, duration], already wrapped by the caller).
        // Interval rule: a point notify at T fires when T is in (prevTime, currTime]; a playhead that
        // starts exactly on T (first evaluation) is handled with `includeStart`.
        // Looping (loop == true and currTime < prevTime) means the playhead wrapped once: the crossed
        // range is (prevTime, duration] then [0, currTime].
        // A state fires Begin when its start is crossed and End when its end (time + duration) is crossed;
        // a state whose window is entered and left in one step fires Begin then End.
        // Events are appended in playhead order. Backwards playback is not supported (no events).
        void Collect(float prevTime, float currTime, float duration, bool loop, bool includeStart,
                     std::vector<AnimNotifyEvent>& outEvents) const;

        // True while `time` is inside the state's window [time, time + duration).
        bool IsStateActive(uint32_t notifyIndex, float time) const;

    private:
        std::vector<AnimNotify> m_notifies;
    };
}

namespace SasamiEngine { using namespace Render; }
