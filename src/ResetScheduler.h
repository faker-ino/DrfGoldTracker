// ResetScheduler - computes when the next automatic session reset is due.
// Pure calculation only (no timers, no I/O) - main.cpp owns comparing the
// result against wall-clock time and actually calling SessionTracker::Reset.
#pragma once

#include <chrono>
#include <string>

enum class AutomaticReset {
    Never,
    OnAddonLoad,
    MinutesAfterUnload,
    OnDailyReset,
    OnWeeklyReset, // general weekly reset (raid/world-boss lockouts) - Monday 07:30 UTC
};

const char* AutomaticResetToString(AutomaticReset value);
AutomaticReset AutomaticResetFromString(const std::string& value, AutomaticReset fallback = AutomaticReset::Never);

// Human-readable label for the options UI combo box.
const char* AutomaticResetLabel(AutomaticReset value);

namespace ResetScheduler {

// nowUtc is the current wall-clock time. minutesUntilResetAfterUnload is
// only consulted for AutomaticReset::MinutesAfterUnload. For Never and
// OnAddonLoad there's no fixed future timestamp to wait for (the check
// happens directly on Load()), so this returns time_point::max() - callers
// must not schedule an automatic reset off that value for those two modes.
std::chrono::system_clock::time_point ComputeNextResetUtc(
    std::chrono::system_clock::time_point nowUtc,
    AutomaticReset mode,
    int minutesUntilResetAfterUnload);

} // namespace ResetScheduler
