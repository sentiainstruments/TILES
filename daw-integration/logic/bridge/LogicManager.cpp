#include "LogicManager.hpp"

#include <utility>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

namespace sentia::logic {

struct LogicManager::Impl {
#ifdef __APPLE__
    id status_item = nullptr;
    id menu = nullptr;
    id item_toggle = nullptr;
    id item_live_loops = nullptr;
    id item_mixer = nullptr;
    id item_monitor = nullptr;
    id target = nullptr;
#else
    int unused = 0;
#endif
};

#ifdef __APPLE__
namespace {

LogicManager* g_active_manager = nullptr;

template <typename Ret, typename... Args>
Ret callObjc(id obj, const char* sel_name, Args... args) {
    using Fn = Ret (*)(id, SEL, Args...);
    Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(objc_msgSend));
    return fn(obj, sel_registerName(sel_name), args...);
}

id nsString(const char* utf8) {
    const id cls = reinterpret_cast<id>(objc_getClass("NSString"));
    return callObjc<id, const char*>(cls, "stringWithUTF8String:", utf8);
}

void objcToggleAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    if (g_active_manager) {
        g_active_manager->toggleLiveLoopsMixer();
    }
}

void objcSelectLiveLoopsAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    if (g_active_manager) {
        g_active_manager->setViewState(LogicManager::ViewState::LiveLoops);
    }
}

void objcSelectMixerAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    if (g_active_manager) {
        g_active_manager->setViewState(LogicManager::ViewState::Mixer);
    }
}

void objcQuitAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    CFRunLoopStop(CFRunLoopGetMain());
}

void objcOpenMonitorAction(id self, SEL cmd, id sender) {
    (void)self;
    (void)cmd;
    (void)sender;
    if (g_active_manager) {
        g_active_manager->triggerOpenMonitor();
    }
}

Class ensureTargetClass() {
    Class existing = objc_getClass("TilesLogicUiTarget");
    if (existing) return existing;

    Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "TilesLogicUiTarget", 0);
    if (!cls) return nullptr;

    class_addMethod(cls, sel_registerName("onToggle:"), reinterpret_cast<IMP>(objcToggleAction), "v@:@");
    class_addMethod(cls, sel_registerName("onLiveLoops:"), reinterpret_cast<IMP>(objcSelectLiveLoopsAction), "v@:@");
    class_addMethod(cls, sel_registerName("onMixer:"), reinterpret_cast<IMP>(objcSelectMixerAction), "v@:@");
    class_addMethod(cls, sel_registerName("onOpenMonitor:"), reinterpret_cast<IMP>(objcOpenMonitorAction), "v@:@");
    class_addMethod(cls, sel_registerName("onQuit:"), reinterpret_cast<IMP>(objcQuitAction), "v@:@");
    objc_registerClassPair(cls);
    return cls;
}

id makeMenuItem(const char* title, const char* action_sel, id target) {
    const id cls = reinterpret_cast<id>(objc_getClass("NSMenuItem"));
    id item = callObjc<id>(cls, "alloc");
    item = callObjc<id, id, SEL, id>(
        item,
        "initWithTitle:action:keyEquivalent:",
        nsString(title),
        action_sel ? sel_registerName(action_sel) : static_cast<SEL>(nullptr),
        nsString("")
    );
    if (target) {
        callObjc<void, id>(item, "setTarget:", target);
    }
    return item;
}

} // namespace
#endif

LogicManager::LogicManager() : impl_(new Impl()) {}

LogicManager::~LogicManager() {
    detachUi();
    delete impl_;
    impl_ = nullptr;
}

void LogicManager::setOnViewChanged(ViewChangedCallback cb) {
    on_view_changed_ = std::move(cb);
}

void LogicManager::setOnOpenMonitor(std::function<void()> cb) {
    on_open_monitor_ = std::move(cb);
}

void LogicManager::triggerOpenMonitor() {
    if (on_open_monitor_) {
        on_open_monitor_();
    }
}

void LogicManager::toggleLiveLoopsMixer() {
    const ViewState next = (state_ == ViewState::LiveLoops) ? ViewState::Mixer : ViewState::LiveLoops;
    setViewState(next);
}

void LogicManager::setViewState(ViewState state) {
    if (state_ == state) {
        updateUiTitle();
        return;
    }
    state_ = state;
    updateUiTitle();
    if (on_view_changed_) {
        on_view_changed_(state_);
    }
}

void LogicManager::syncFromBridgePage(tiles_logic_page_t page) {
    if (page == TILES_PAGE_LIVE_LOOPS) {
        state_ = ViewState::LiveLoops;
    } else if (page == TILES_PAGE_MIXER) {
        state_ = ViewState::Mixer;
    } else if (page == TILES_PAGE_SMART_CONTROLS) {
        state_ = ViewState::SmartControls;
    }
    updateUiTitle();
}

const char* LogicManager::viewStateLabel() const noexcept {
    switch (state_) {
        case ViewState::LiveLoops:
            return "Live Loops";
        case ViewState::SmartControls:
            return "Smart Controls";
        case ViewState::Mixer:
            return "Mixer";
    }
    return "Live Loops";
}

bool LogicManager::attachUi() {
    if (ui_attached_) return true;

#ifdef __APPLE__
    const id nsapp_cls = reinterpret_cast<id>(objc_getClass("NSApplication"));
    const id statusbar_cls = reinterpret_cast<id>(objc_getClass("NSStatusBar"));
    const id menu_cls = reinterpret_cast<id>(objc_getClass("NSMenu"));
    if (!nsapp_cls || !statusbar_cls || !menu_cls) {
        return false;
    }

    const id app = callObjc<id>(nsapp_cls, "sharedApplication");
    if (!app) return false;
    /* NSApplicationActivationPolicyAccessory (1) keeps bridge out of Dock while driving menu bar UI. */
    callObjc<bool, long>(app, "setActivationPolicy:", 1L);
    callObjc<void>(app, "finishLaunching");

    const id sys_bar = callObjc<id>(statusbar_cls, "systemStatusBar");
    if (!sys_bar) return false;

    /* NSVariableStatusItemLength == -1.0 */
    impl_->status_item = callObjc<id, double>(sys_bar, "statusItemWithLength:", -1.0);
    if (!impl_->status_item) return false;
    callObjc<id>(impl_->status_item, "retain");

    Class target_cls = ensureTargetClass();
    if (target_cls) {
        impl_->target = callObjc<id>(callObjc<id>(reinterpret_cast<id>(target_cls), "alloc"), "init");
    }

    impl_->menu = callObjc<id>(callObjc<id>(menu_cls, "alloc"), "initWithTitle:", nsString("SENTIA TILES"));
    impl_->item_toggle = makeMenuItem("Toggle Live Loops / Mixer", "onToggle:", impl_->target);
    impl_->item_live_loops = makeMenuItem("Live Loops", "onLiveLoops:", impl_->target);
    impl_->item_mixer = makeMenuItem("Mixer", "onMixer:", impl_->target);
    impl_->item_monitor = makeMenuItem("Open MPE MIDI Monitor\u2026", "onOpenMonitor:", impl_->target);
    const id item_quit = makeMenuItem("Quit Bridge", "onQuit:", impl_->target);
    const id sep = callObjc<id>(reinterpret_cast<id>(objc_getClass("NSMenuItem")), "separatorItem");

    callObjc<void, id>(impl_->menu, "addItem:", impl_->item_toggle);
    callObjc<void, id>(impl_->menu, "addItem:", sep);
    callObjc<void, id>(impl_->menu, "addItem:", impl_->item_live_loops);
    callObjc<void, id>(impl_->menu, "addItem:", impl_->item_mixer);
    callObjc<void, id>(impl_->menu, "addItem:", callObjc<id>(reinterpret_cast<id>(objc_getClass("NSMenuItem")), "separatorItem"));
    callObjc<void, id>(impl_->menu, "addItem:", impl_->item_monitor);
    callObjc<void, id>(impl_->menu, "addItem:", callObjc<id>(reinterpret_cast<id>(objc_getClass("NSMenuItem")), "separatorItem"));
    callObjc<void, id>(impl_->menu, "addItem:", item_quit);

    /* NSMenu retains added items; release initial alloc retains cleanly. */
    callObjc<void>(impl_->item_toggle, "release");
    callObjc<void>(impl_->item_live_loops, "release");
    callObjc<void>(impl_->item_mixer, "release");
    callObjc<void>(impl_->item_monitor, "release");
    callObjc<void>(item_quit, "release");

    callObjc<void, id>(impl_->status_item, "setMenu:", impl_->menu);
    g_active_manager = this;
    ui_attached_ = true;
    updateUiTitle();
    return true;
#else
    ui_attached_ = true;
    return true;
#endif
}

void LogicManager::detachUi() {
    if (!ui_attached_) return;

#ifdef __APPLE__
    if (g_active_manager == this) {
        g_active_manager = nullptr;
    }
    if (impl_->status_item) {
        const id statusbar_cls = reinterpret_cast<id>(objc_getClass("NSStatusBar"));
        if (statusbar_cls) {
            const id sys_bar = callObjc<id>(statusbar_cls, "systemStatusBar");
            if (sys_bar) {
                callObjc<void, id>(sys_bar, "removeStatusItem:", impl_->status_item);
            }
        }
        callObjc<void>(impl_->status_item, "release");
        impl_->status_item = nullptr;
    }
    if (impl_->menu) {
        callObjc<void>(impl_->menu, "release");
        impl_->menu = nullptr;
    }
    if (impl_->target) {
        callObjc<void>(impl_->target, "release");
        impl_->target = nullptr;
    }
#endif
    ui_attached_ = false;
}

void LogicManager::updateUiTitle() {
#ifdef __APPLE__
    if (!ui_attached_ || !impl_->status_item) return;

    const char* title = (state_ == ViewState::Mixer) ? "TILES: Mixer"
                        : (state_ == ViewState::SmartControls) ? "TILES: Smart"
                        : "TILES: Live Loops";
    const id button = callObjc<id>(impl_->status_item, "button");
    if (button) {
        callObjc<void, id>(button, "setTitle:", nsString(title));
    }

    if (impl_->item_live_loops) {
        callObjc<void, long>(impl_->item_live_loops, "setState:", state_ == ViewState::LiveLoops ? 1L : 0L);
    }
    if (impl_->item_mixer) {
        callObjc<void, long>(impl_->item_mixer, "setState:", state_ == ViewState::Mixer ? 1L : 0L);
    }
#endif
}

} // namespace sentia::logic
