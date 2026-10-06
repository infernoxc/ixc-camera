#pragma once

// When the app sends a setting change to IXC Camera, and when it writes the profile file.
//
//   * Publish (the small active-profile file the camera service watches): at most one every
//     kPublishMs while a slider is being dragged. The first change of a burst arms the timer; later
//     changes ride on it (the timer reads the profile when it fires, so the newest value is sent).
//     A change after the timer fired arms it again, so the final value is never lost. Apps using
//     IXC Camera follow a drag live (about 25 updates a second) instead of only after it stops.
//   * Save (the user's profile in %LOCALAPPDATA%): debounced, kSaveMs after the last change.
//
// Pure logic; the window owns the actual timers.

namespace ixc {

// Mouse-wheel latching for sliders inside a scrolling panel: the wheel adjusts the slider under the
// pointer, unless the panel itself scrolled in the last kWheelLatchMs (the user is scrolling past
// it) or Shift is held. True = the wheel goes to the slider.
inline constexpr unsigned kWheelLatchMs = 600;
inline bool WheelAdjustsSlider(unsigned long long nowMs, unsigned long long lastPanelScrollMs, bool shift) {
    if (shift) return false;
    return lastPanelScrollMs == 0 || nowMs - lastPanelScrollMs >= kWheelLatchMs;
}

class SettingsSync {
public:
    static constexpr unsigned kPublishMs = 40;
    static constexpr unsigned kSaveMs = 700;

    // A setting changed. True when the publish timer must be armed now (none pending).
    bool OnChange() {
        savePending_ = true;
        if (publishArmed_) return false;
        publishArmed_ = true;
        return true;
    }
    // The publish timer fired (publish the current profile now).
    void OnPublished() { publishArmed_ = false; }
    // The save timer fired, or the profile was saved by another path.
    void OnSaved() { savePending_ = false; }

    bool PublishArmed() const { return publishArmed_; }
    bool SavePending() const { return savePending_; }

private:
    bool publishArmed_ = false;
    bool savePending_ = false;
};

}  // namespace ixc
