#pragma once

#include <cstdint>
#include <functional>

#include "tiles_logic_core.h"

namespace sentia::logic {

/* Dedicated C++ UI manager for toggling between 'Live Loops' and 'Mixer' view states. */
class LogicManager {
public:
    enum class ViewState : uint8_t {
        LiveLoops = TILES_PAGE_LIVE_LOOPS,
        SmartControls = TILES_PAGE_SMART_CONTROLS,
        Mixer = TILES_PAGE_MIXER,
    };

    using ViewChangedCallback = std::function<void(ViewState)>;

    LogicManager();
    ~LogicManager();

    LogicManager(const LogicManager&) = delete;
    LogicManager& operator=(const LogicManager&) = delete;

    void setOnViewChanged(ViewChangedCallback cb);
    void setOnOpenMonitor(std::function<void()> cb);
    void triggerOpenMonitor();

    /* Toggles cleanly between 'Live Loops' and 'Mixer' view states. */
    void toggleLiveLoopsMixer();

    /* Sets an explicit view state and notifies attached bridge/UI listeners. */
    void setViewState(ViewState state);

    /* Mirrors page changes initiated from TILES hardware or Logic Pro into the UI. */
    void syncFromBridgePage(tiles_logic_page_t page);

    ViewState viewState() const noexcept { return state_; }
    const char* viewStateLabel() const noexcept;

    /* Attaches/detaches the native macOS menu bar UI (no-op when headless or non-Apple). */
    bool attachUi();
    void detachUi();
    bool isUiAttached() const noexcept { return ui_attached_; }

private:
    void updateUiTitle();

    ViewState state_ = ViewState::LiveLoops;
    ViewChangedCallback on_view_changed_;
    std::function<void()> on_open_monitor_;
    bool ui_attached_ = false;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace sentia::logic
