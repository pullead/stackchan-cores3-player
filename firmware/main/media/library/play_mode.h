#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

// Cycled by the transport button, in the upstream order: sequential stops at the
// end of the list, repeat-all wraps, repeat-one replays the same track, shuffle
// picks at random.
enum class PlayMode : std::uint8_t { Sequential, RepeatAll, RepeatOne, Shuffle };

// What to do when a track runs out.  This is deliberately a different question
// from what a manual next/prev does: upstream keeps manual next/prev moving even
// in repeat-one, while the automatic case repeats.
enum class FinishAction : std::uint8_t { Stop, RestartTrack, AdvanceTrack };

// `at_end` says whether the track that just finished is the last one in the view
// being played from, which only sequential playback cares about.
constexpr FinishAction action_after_finish(PlayMode mode, bool at_end) noexcept {
    switch (mode) {
        case PlayMode::RepeatOne:
            return FinishAction::RestartTrack;
        case PlayMode::Sequential:
            return at_end ? FinishAction::Stop : FinishAction::AdvanceTrack;
        case PlayMode::RepeatAll:
        case PlayMode::Shuffle:
            return FinishAction::AdvanceTrack;
    }
    return FinishAction::Stop;
}

// Whether `track` is the last entry of the view.  A track that is not in the view
// at all (un-favourited while playing, for instance) is not the end of it.
inline bool is_last_in_view(const std::vector<std::size_t>& view, std::size_t track) noexcept {
    return !view.empty() && view.back() == track;
}

}  // namespace media
