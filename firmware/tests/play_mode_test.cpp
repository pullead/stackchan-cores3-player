// Host tests for the end-of-track policy and the view helper it uses.

#include "media/library/play_mode.h"

#include <cassert>
#include <cstddef>
#include <vector>

using namespace media;

int main() {
    // Repeat one repeats, wherever the track sits in the view.
    assert(action_after_finish(PlayMode::RepeatOne, false) == FinishAction::RestartTrack);
    assert(action_after_finish(PlayMode::RepeatOne, true) == FinishAction::RestartTrack);

    // Sequential advances until the view ends, then stops.
    assert(action_after_finish(PlayMode::Sequential, false) == FinishAction::AdvanceTrack);
    assert(action_after_finish(PlayMode::Sequential, true) == FinishAction::Stop);

    // The two wrapping modes always have somewhere to go.
    assert(action_after_finish(PlayMode::RepeatAll, false) == FinishAction::AdvanceTrack);
    assert(action_after_finish(PlayMode::RepeatAll, true) == FinishAction::AdvanceTrack);
    assert(action_after_finish(PlayMode::Shuffle, false) == FinishAction::AdvanceTrack);
    assert(action_after_finish(PlayMode::Shuffle, true) == FinishAction::AdvanceTrack);

    // Only the last entry of the view is the end of it, and a track that is not
    // in the view at all is not the end of anything.
    const std::vector<std::size_t> view = {5, 9, 2};
    assert(is_last_in_view(view, 2));
    assert(!is_last_in_view(view, 5));
    assert(!is_last_in_view(view, 9));
    assert(!is_last_in_view(view, 7));

    assert(!is_last_in_view({}, 0));
    assert(is_last_in_view({3}, 3));
    assert(!is_last_in_view({3}, 4));

    return 0;
}
