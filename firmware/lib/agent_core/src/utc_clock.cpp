#include "chatesp/utc_clock.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace chatesp {
namespace agent {
namespace {

constexpr std::uint64_t kSecondsPerDay = 86'400;

bool decimal_pair(const char *text, unsigned &value) {
    if (text[0] < '0' || text[0] > '9' ||
        text[1] < '0' || text[1] > '9') {
        return false;
    }
    value = static_cast<unsigned>(text[0] - '0') * 10U +
        static_cast<unsigned>(text[1] - '0');
    return true;
}

bool decimal_year(const char *text, unsigned &value) {
    value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        value = value * 10U + static_cast<unsigned>(text[index] - '0');
    }
    return true;
}

unsigned month_number(const char *text) {
    constexpr std::array<const char *, 12> names{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (std::memcmp(text, names[index], 3) == 0) {
            return static_cast<unsigned>(index + 1);
        }
    }
    return 0;
}

bool leap_year(unsigned year) {
    return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

unsigned days_in_month(unsigned year, unsigned month) {
    constexpr std::array<unsigned, 12> days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month == 0 || month > days.size()) {
        return 0;
    }
    return month == 2 && leap_year(year) ? 29U : days[month - 1];
}

// Returns the number of days since 1970-01-01. The input is validated first.
std::int64_t days_from_civil(unsigned year, unsigned month, unsigned day) {
    int adjusted_year = static_cast<int>(year) - (month <= 2 ? 1 : 0);
    const int era = adjusted_year / 400;
    const unsigned year_of_era =
        static_cast<unsigned>(adjusted_year - era * 400);
    const unsigned month_prime = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year =
        (153U * month_prime + 2U) / 5U +
        day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
        day_of_year;
    return static_cast<std::int64_t>(era) * 146'097 +
        static_cast<std::int64_t>(day_of_era) - 719'468;
}

void civil_from_days(
    std::uint64_t days_since_epoch, unsigned &year, unsigned &month,
    unsigned &day) {
    const std::int64_t shifted =
        static_cast<std::int64_t>(days_since_epoch) + 719'468;
    const std::int64_t era = shifted / 146'097;
    const unsigned day_of_era =
        static_cast<unsigned>(shifted - era * 146'097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1'460U + day_of_era / 36'524U -
         day_of_era / 146'096U) /
        365U;
    int civil_year = static_cast<int>(year_of_era) +
        static_cast<int>(era * 400);
    const unsigned day_of_year =
        day_of_era -
        (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
    day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
    month = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
    civil_year += month <= 2U ? 1 : 0;
    year = static_cast<unsigned>(civil_year);
}

}  // namespace

bool UtcClock::update_from_http_date(
    const char *value, std::size_t size, std::uint32_t observed_at_ms) {
    // IMF-fixdate: "Sat, 08 Aug 2026 12:34:56 GMT"
    if (value == nullptr || size != 29 || value[3] != ',' || value[4] != ' ' ||
        value[7] != ' ' || value[11] != ' ' || value[16] != ' ' ||
        value[19] != ':' || value[22] != ':' || value[25] != ' ' ||
        std::memcmp(value + 26, "GMT", 3) != 0) {
        return false;
    }

    unsigned day = 0;
    unsigned year = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    const unsigned month = month_number(value + 8);
    if (!decimal_pair(value + 5, day) || !decimal_year(value + 12, year) ||
        !decimal_pair(value + 17, hour) ||
        !decimal_pair(value + 20, minute) ||
        !decimal_pair(value + 23, second) || year < 2020 || year > 9999 ||
        month == 0 || day == 0 || day > days_in_month(year, month) ||
        hour > 23 || minute > 59 || second > 60) {
        return false;
    }

    const std::int64_t days = days_from_civil(year, month, day);
    if (days < 0) {
        return false;
    }
    observed_epoch_seconds_ = static_cast<std::uint64_t>(days) * kSecondsPerDay +
        static_cast<std::uint64_t>(hour) * 3'600U +
        static_cast<std::uint64_t>(minute) * 60U +
        (second == 60 ? 59U : second);
    observed_at_ms_ = observed_at_ms;
    valid_ = true;
    return true;
}

bool UtcClock::update_from_epoch_seconds(
    std::uint64_t epoch_seconds,
    std::int16_t utc_offset_minutes,
    std::uint32_t observed_at_ms) {
    if (epoch_seconds < 1'577'836'800ULL ||
        epoch_seconds > 253'402'300'799ULL ||
        utc_offset_minutes < -840 || utc_offset_minutes > 840) {
        return false;
    }
    observed_epoch_seconds_ = epoch_seconds;
    observed_at_ms_ = observed_at_ms;
    utc_offset_minutes_ = utc_offset_minutes;
    has_utc_offset_ = true;
    valid_ = true;
    return true;
}

bool UtcClock::current_seconds(
    std::uint32_t now_ms, bool require_utc_offset,
    std::uint64_t &output) const {
    if (!valid_ || (require_utc_offset && !has_utc_offset_)) {
        return false;
    }
    output = observed_epoch_seconds_ +
        static_cast<std::uint32_t>(now_ms - observed_at_ms_) / 1'000U;
    if (!has_utc_offset_) {
        return true;
    }
    const std::int64_t local_seconds = static_cast<std::int64_t>(output) +
        static_cast<std::int64_t>(utc_offset_minutes_) * 60;
    if (local_seconds < 0) {
        return false;
    }
    output = static_cast<std::uint64_t>(local_seconds);
    return true;
}

bool UtcClock::current_minute(
    std::uint32_t now_ms, UtcMinuteText &output) const {
    output.clear();
    std::uint64_t current_seconds_value = 0;
    if (!current_seconds(now_ms, false, current_seconds_value)) {
        return false;
    }
    const std::uint64_t days = current_seconds_value / kSecondsPerDay;
    const unsigned seconds_in_day =
        static_cast<unsigned>(current_seconds_value % kSecondsPerDay);
    const unsigned hour = seconds_in_day / 3'600U;
    const unsigned minute = (seconds_in_day % 3'600U) / 60U;
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);

    char formatted[27]{};
    int size = 0;
    if (has_utc_offset_) {
        const int offset = utc_offset_minutes_;
        const unsigned magnitude =
            static_cast<unsigned>(offset < 0 ? -offset : offset);
        size = std::snprintf(
            formatted, sizeof(formatted),
            "%04u-%02u-%02u %02u:%02u UTC%c%02u:%02u",
            year, month, day, hour, minute, offset < 0 ? '-' : '+',
            magnitude / 60U, magnitude % 60U);
    } else {
        size = std::snprintf(
            formatted, sizeof(formatted), "%04u-%02u-%02u %02u:%02u UTC",
            year, month, day, hour, minute);
    }
    return size > 0 && static_cast<std::size_t>(size) <= output.capacity() &&
        output.assign(formatted, static_cast<std::size_t>(size));
}

bool UtcClock::current_local_time(
    std::uint32_t now_ms, LocalTimeOfDay &output) const {
    output = {};
    std::uint64_t current_seconds_value = 0;
    if (!current_seconds(now_ms, true, current_seconds_value)) {
        return false;
    }
    const unsigned seconds_in_day = static_cast<unsigned>(
        current_seconds_value % kSecondsPerDay);
    output.hour = static_cast<std::uint8_t>(seconds_in_day / 3'600U);
    output.minute = static_cast<std::uint8_t>(
        (seconds_in_day % 3'600U) / 60U);
    output.second = static_cast<std::uint8_t>(seconds_in_day % 60U);
    return true;
}

bool UtcClock::valid_minute_text(const char *value) {
    if (value == nullptr ||
        (std::strlen(value) != 20 && std::strlen(value) != 26) ||
        value[4] != '-' ||
        value[7] != '-' || value[10] != ' ' || value[13] != ':' ||
        std::memcmp(value + 16, " UTC", 4) != 0) {
        return false;
    }
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    const std::size_t size = std::strlen(value);
    bool offset_valid = true;
    if (size == 26) {
        unsigned offset_hour = 0;
        unsigned offset_minute = 0;
        offset_valid = (value[20] == '+' || value[20] == '-') &&
            value[23] == ':' && decimal_pair(value + 21, offset_hour) &&
            decimal_pair(value + 24, offset_minute) &&
            offset_hour <= 14 && offset_minute <= 59 &&
            (offset_hour != 14 || offset_minute == 0);
    }
    return offset_valid && decimal_year(value, year) && decimal_pair(value + 5, month) &&
        decimal_pair(value + 8, day) && decimal_pair(value + 11, hour) &&
        decimal_pair(value + 14, minute) && year >= 2020 && year <= 9999 &&
        month >= 1 && month <= 12 && day >= 1 &&
        day <= days_in_month(year, month) && hour <= 23 && minute <= 59;
}

}  // namespace agent
}  // namespace chatesp
