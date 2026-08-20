#include "ResetScheduler.h"

#include <ctime>

const char* AutomaticResetToString(AutomaticReset value)
{
    switch (value) {
        case AutomaticReset::Never:               return "never";
        case AutomaticReset::OnAddonLoad:         return "on_addon_load";
        case AutomaticReset::MinutesAfterUnload:  return "minutes_after_unload";
        case AutomaticReset::OnDailyReset:        return "on_daily_reset";
        case AutomaticReset::OnWeeklyReset:       return "on_weekly_reset";
    }
    return "never";
}

AutomaticReset AutomaticResetFromString(const std::string& value, AutomaticReset fallback)
{
    if (value == "never") return AutomaticReset::Never;
    if (value == "on_addon_load") return AutomaticReset::OnAddonLoad;
    if (value == "minutes_after_unload") return AutomaticReset::MinutesAfterUnload;
    if (value == "on_daily_reset") return AutomaticReset::OnDailyReset;
    if (value == "on_weekly_reset") return AutomaticReset::OnWeeklyReset;
    // "on_weekly_na_wvw_reset"/"on_weekly_eu_wvw_reset"/"on_weekly_map_bonus_reset"
    // are values from a settings.json written before those modes were
    // removed - just fall back rather than failing to load.
    return fallback; // unknown/malformed setting - keep whatever the caller had
}

const char* AutomaticResetLabel(AutomaticReset value)
{
    switch (value) {
        case AutomaticReset::Never:               return "Never";
        case AutomaticReset::OnAddonLoad:         return "On addon load";
        case AutomaticReset::MinutesAfterUnload:  return "N minutes after addon unload";
        case AutomaticReset::OnDailyReset:        return "Daily reset (00:00 UTC)";
        case AutomaticReset::OnWeeklyReset:       return "Weekly reset (Monday 07:30 UTC)";
    }
    return "Unknown";
}

namespace {

// _mkgmtime/gmtime_s are MSVC CRT extensions (UTC counterparts of the
// non-reentrant/local-time-only std::mktime/gmtime) - fine here since this
// project only ever targets MSVC/Windows (see CLAUDE.md).
std::chrono::system_clock::time_point TodayMidnightUtc(std::chrono::system_clock::time_point nowUtc)
{
    const std::time_t nowTimeT = std::chrono::system_clock::to_time_t(nowUtc);
    std::tm nowTm{};
    gmtime_s(&nowTm, &nowTimeT);
    nowTm.tm_hour = 0;
    nowTm.tm_min = 0;
    nowTm.tm_sec = 0;
    return std::chrono::system_clock::from_time_t(_mkgmtime(&nowTm));
}

int UtcWeekday(std::chrono::system_clock::time_point utc) // 0=Sunday ... 6=Saturday, matches std::tm::tm_wday
{
    const std::time_t timeT = std::chrono::system_clock::to_time_t(utc);
    std::tm tm{};
    gmtime_s(&tm, &timeT);
    return tm.tm_wday;
}

std::chrono::system_clock::time_point NextDailyResetUtc(std::chrono::system_clock::time_point nowUtc)
{
    // Daily reset is always at the NEXT 00:00 UTC - today's midnight has
    // already passed by definition of "now", so this is always in the future.
    return TodayMidnightUtc(nowUtc) + std::chrono::hours(24);
}

std::chrono::system_clock::time_point NextWeeklyResetUtc(
    std::chrono::system_clock::time_point nowUtc, int resetWeekday, int resetHourUtc, int resetMinuteUtc = 0)
{
    const auto todayMidnightUtc = TodayMidnightUtc(nowUtc);
    const int daysUntilResetWeekday = (resetWeekday - UtcWeekday(nowUtc) + 7) % 7;
    auto candidate = todayMidnightUtc + std::chrono::hours(24) * daysUntilResetWeekday
                                       + std::chrono::hours(resetHourUtc)
                                       + std::chrono::minutes(resetMinuteUtc);
    if (candidate <= nowUtc) {
        candidate += std::chrono::hours(24 * 7);
    }
    return candidate;
}

} // namespace

namespace ResetScheduler {

std::chrono::system_clock::time_point ComputeNextResetUtc(
    std::chrono::system_clock::time_point nowUtc, AutomaticReset mode, int minutesUntilResetAfterUnload)
{
    switch (mode) {
        case AutomaticReset::Never:
        case AutomaticReset::OnAddonLoad:
            return std::chrono::system_clock::time_point::max();
        case AutomaticReset::MinutesAfterUnload:
            return nowUtc + std::chrono::minutes(minutesUntilResetAfterUnload);
        case AutomaticReset::OnDailyReset:
            return NextDailyResetUtc(nowUtc);
        case AutomaticReset::OnWeeklyReset:
            return NextWeeklyResetUtc(nowUtc, 1 /*Monday*/, 7, 30);
    }
    return std::chrono::system_clock::time_point::max();
}

} // namespace ResetScheduler
