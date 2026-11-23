#include "../bridge/LogicManager.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void testToggleBetweenLiveLoopsAndMixer() {
    sentia::logic::LogicManager mgr;
    std::vector<sentia::logic::LogicManager::ViewState> events;

    mgr.setOnViewChanged([&events](sentia::logic::LogicManager::ViewState s) {
        events.push_back(s);
    });

    assert(mgr.viewState() == sentia::logic::LogicManager::ViewState::LiveLoops);
    assert(std::strcmp(mgr.viewStateLabel(), "Live Loops") == 0);

    mgr.toggleLiveLoopsMixer();
    assert(mgr.viewState() == sentia::logic::LogicManager::ViewState::Mixer);
    assert(std::strcmp(mgr.viewStateLabel(), "Mixer") == 0);
    assert(events.size() == 1u);
    assert(events[0] == sentia::logic::LogicManager::ViewState::Mixer);

    mgr.toggleLiveLoopsMixer();
    assert(mgr.viewState() == sentia::logic::LogicManager::ViewState::LiveLoops);
    assert(std::strcmp(mgr.viewStateLabel(), "Live Loops") == 0);
    assert(events.size() == 2u);
    assert(events[1] == sentia::logic::LogicManager::ViewState::LiveLoops);

    /* Setting identical state should not fire redundant callback events. */
    mgr.setViewState(sentia::logic::LogicManager::ViewState::LiveLoops);
    assert(events.size() == 2u);

    /* Bridge hardware/MIDI sync updates UI label without re-triggering onViewChanged. */
    mgr.syncFromBridgePage(TILES_PAGE_SMART_CONTROLS);
    assert(mgr.viewState() == sentia::logic::LogicManager::ViewState::SmartControls);
    assert(std::strcmp(mgr.viewStateLabel(), "Smart Controls") == 0);
    assert(events.size() == 2u);

    /* Toggling from Smart Controls transitions cleanly to Live Loops. */
    mgr.toggleLiveLoopsMixer();
    assert(mgr.viewState() == sentia::logic::LogicManager::ViewState::LiveLoops);
    assert(events.size() == 3u);
}

} // namespace

int main() {
    testToggleBetweenLiveLoopsAndMixer();
    std::printf("All SENTIA TILES LogicManager tests passed.\n");
    return 0;
}
