#include "au_radio_table.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

// The generated rows are included here so tests can substitute a fixture table.
#ifndef AU_RADIO_DATA_INC
#define AU_RADIO_DATA_INC "au_radio_table_data.inc"
#endif

namespace au_radio {

namespace data {
#include AU_RADIO_DATA_INC
}  // namespace data

namespace {

constexpr double kEarthRadiusKm = 6371.0;
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

bool equal_ci(const char *a, const char *b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

double distance_km(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = lat1 * kDegreesToRadians;
    const double phi2 = lat2 * kDegreesToRadians;
    const double dphi = phi2 - phi1;
    const double dlambda = (lon2 - lon1) * kDegreesToRadians;
    const double h = std::sin(dphi / 2) * std::sin(dphi / 2) +
                     std::cos(phi1) * std::cos(phi2) * std::sin(dlambda / 2) * std::sin(dlambda / 2);
    return 2 * kEarthRadiusKm * std::asin(std::min(1.0, std::sqrt(h)));
}

}  // namespace

std::size_t area_count() { return data::kAreaCount; }
std::size_t station_count() { return data::kStationCount; }
const Area &area_at(std::size_t index) { return data::kAreas[index]; }
const Station &station_at(std::size_t index) { return data::kStations[index]; }
const char *text(uint16_t offset) { return data::kPool + offset; }
const char *area_name(std::size_t area) { return text(area_at(area).name); }

const char *band_label(uint8_t band) {
    switch (band) {
        case kBandFm: return "FM";
        case kBandAm: return "AM";
        case kBandDab: return "DAB+";
        default: return "";
    }
}

bool area_by_name(const char *name, std::size_t *area) {
    if (!name || !name[0] || !area) return false;
    for (std::size_t i = 0; i < area_count(); ++i) {
        if (equal_ci(name, text(area_at(i).name))) {
            *area = i;
            return true;
        }
    }
    return false;
}

bool nearest_area(double lat, double lon, std::size_t *area, double *km) {
    if (!area || !std::isfinite(lat) || !std::isfinite(lon)) return false;
    bool found = false;
    double best = 0;
    std::size_t nearest = 0;
    // Densest area within kPreferKm: most stations, then the nearer one.
    bool near_found = false;
    std::size_t dense = 0;
    double dense_km = 0;
    for (std::size_t i = 0; i < area_count(); ++i) {
        const Area &row = area_at(i);
        const double d = distance_km(lat, lon, row.lat_e4 / 1e4, row.lon_e4 / 1e4);
        if (!found || d < best) {
            found = true;
            best = d;
            nearest = i;
        }
        if (d <= kPreferKm &&
            (!near_found || row.count > area_at(dense).count ||
             (row.count == area_at(dense).count && d < dense_km))) {
            near_found = true;
            dense = i;
            dense_km = d;
        }
    }
    if (!found) return false;
    if (near_found) {
        *area = dense;
        if (km) *km = dense_km;
    } else {
        *area = nearest;
        if (km) *km = best;
    }
    return true;
}

bool area_for_profile(const char *city, const char *region, const char *lat, const char *lon,
                      std::size_t *area) {
    if (!area) return false;
    if (area_by_name(city, area) || area_by_name(region, area)) return true;
    if (!lat || !lon || !lat[0] || !lon[0]) return false;
    char *lat_end = nullptr;
    char *lon_end = nullptr;
    const double la = std::strtod(lat, &lat_end);
    const double lo = std::strtod(lon, &lon_end);
    if (lat_end == lat || lon_end == lon) return false;
    double km = 0;
    return nearest_area(la, lo, area, &km) && km <= kMaxSelectKm;
}

std::size_t collect_stations(std::size_t area, uint8_t band_mask, uint16_t *out, std::size_t capacity) {
    if (!out || area >= area_count()) return 0;
    const Area &row = area_at(area);
    std::size_t n = 0;
    for (std::size_t i = row.first; i < std::size_t(row.first) + row.count; ++i) {
        if (n >= capacity || n >= kMaxListed) break;
        if (!(band_mask & (1u << station_at(i).band))) continue;
        out[n++] = static_cast<uint16_t>(i);
    }
    return n;
}

namespace {

constexpr const char *kStateCodes[kStateCount] = {"NSW", "VIC", "QLD", "SA", "WA", "TAS", "ACT", "NT"};

struct Capital {
    const char *name;
    const char *state;
};
constexpr Capital kCapitals[] = {
    {"Sydney", "NSW"}, {"Melbourne", "VIC"}, {"Brisbane", "QLD"}, {"Adelaide", "SA"},
    {"Perth", "WA"},   {"Hobart", "TAS"},    {"Canberra", "ACT"}, {"Darwin", "NT"},
};

bool is_capital(std::size_t area) {
    const Area &row = area_at(area);
    for (const Capital &capital : kCapitals) {
        if (equal_ci(capital.name, text(row.name)) && equal_ci(capital.state, text(row.state))) return true;
    }
    return false;
}

// Picker rows: the capital first, then the other areas A-Z (case-insensitive).
bool picker_area_before(uint16_t a, uint16_t b) {
    const bool cap_a = is_capital(a);
    const bool cap_b = is_capital(b);
    if (cap_a != cap_b) return cap_a;
    const char *x = area_name(a);
    const char *y = area_name(b);
    for (;; ++x, ++y) {
        const int cx = std::tolower(static_cast<unsigned char>(*x));
        const int cy = std::tolower(static_cast<unsigned char>(*y));
        if (cx != cy) return cx < cy;
        if (cx == '\0') return false;
    }
}

// Bit order for next_band(): FM -> AM -> DAB+ -> FM.
constexpr uint8_t kBandCycle[3] = {kBandFm, kBandAm, kBandDab};

}  // namespace

const char *state_code(std::size_t index) { return index < kStateCount ? kStateCodes[index] : ""; }

std::size_t areas_in_state(const char *state, uint16_t *out, std::size_t capacity) {
    if (!state || !state[0] || !out) return 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < area_count() && n < capacity; ++i) {
        if (equal_ci(state, text(area_at(i).state))) out[n++] = static_cast<uint16_t>(i);
    }
    return n;
}

uint8_t bands_in_area(std::size_t area) {
    if (area >= area_count()) return 0;
    const Area &row = area_at(area);
    uint8_t mask = 0;
    for (std::size_t i = row.first; i < std::size_t(row.first) + row.count; ++i) {
        mask = static_cast<uint8_t>(mask | (1u << station_at(i).band));
    }
    return mask;
}

uint8_t first_band(uint8_t band_mask) {
    for (uint8_t band : kBandCycle) {
        if (band_mask & (1u << band)) return band;
    }
    return kBandFm;
}

uint8_t next_band(uint8_t current, uint8_t band_mask) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        if (kBandCycle[i] == current) at = i;
    }
    for (std::size_t step = 1; step <= 3; ++step) {
        const uint8_t band = kBandCycle[(at + step) % 3];
        if (band_mask & (1u << band)) return band;
    }
    return current;
}

void format_dial_readout(const Station &row, char *out, std::size_t capacity) {
    if (!out || capacity == 0) return;
    switch (row.band) {
        case kBandFm:
            std::snprintf(out, capacity, "%u.%u", unsigned(row.frequency / 10),
                          unsigned(row.frequency % 10));
            break;
        case kBandAm:
            std::snprintf(out, capacity, "%u kHz", unsigned(row.frequency));
            break;
        default: {
            // Simulcasts of FM and AM services have no block, so they read as DAB+.
            const char *block = text(row.dab_block);
            std::snprintf(out, capacity, "%s", block[0] ? block : "DAB+");
            break;
        }
    }
}

bool format_area_choice(std::size_t area, char *out, std::size_t capacity) {
    if (!out || capacity == 0 || area >= area_count()) return false;
    const Area &row = area_at(area);
    std::snprintf(out, capacity, "%s, %s", text(row.name), text(row.state));
    return true;
}

bool area_for_choice(const char *choice, std::size_t *area) {
    if (!choice || !choice[0] || !area) return false;
    const char *comma = std::strrchr(choice, ',');
    char name[48] = {};
    std::size_t name_len = comma ? std::size_t(comma - choice) : std::strlen(choice);
    if (name_len == 0 || name_len >= sizeof(name)) return false;
    std::memcpy(name, choice, name_len);
    while (name_len && name[name_len - 1] == ' ') name[--name_len] = '\0';
    const char *state = comma ? comma + 1 : nullptr;
    while (state && *state == ' ') ++state;
    if (state && !state[0]) state = nullptr;
    for (std::size_t i = 0; i < area_count(); ++i) {
        const Area &row = area_at(i);
        if (!equal_ci(name, text(row.name))) continue;
        if (state && !equal_ci(state, text(row.state))) continue;
        *area = i;
        return true;
    }
    return false;
}

bool area_for_selection(const char *choice, const char *city, const char *region, const char *lat,
                        const char *lon, std::size_t *area) {
    if (area_for_choice(choice, area)) return true;
    return area_for_profile(city, region, lat, lon, area);
}

void AreaPicker::open() {
    level_ = Level::States;
    selected_ = 0;
    state_count_ = 0;
    area_count_ = 0;
    for (std::size_t i = 0; i < kStateCount; ++i) {
        uint16_t probe[1];
        if (areas_in_state(kStateCodes[i], probe, 1) > 0) states_[state_count_++] = static_cast<uint8_t>(i);
    }
}

std::size_t AreaPicker::count() const {
    return level_ == Level::States ? state_count_ : area_count_ + 1;
}

void AreaPicker::move(int delta) {
    const std::size_t rows = count();
    if (rows == 0) {
        selected_ = 0;
        return;
    }
    if (delta < 0) {
        const std::size_t step = static_cast<std::size_t>(-delta);
        selected_ = selected_ > step ? selected_ - step : 0;
    } else {
        selected_ = std::min(rows - 1, selected_ + static_cast<std::size_t>(delta));
    }
}

const char *AreaPicker::label(std::size_t i) const {
    if (level_ == Level::States) return i < state_count_ ? kStateCodes[states_[i]] : "";
    if (i < area_count_) return area_name(areas_[i]);
    return i == area_count_ ? kAutoLabel : "";
}

const char *AreaPicker::title() const {
    return level_ == Level::States ? "Area" : state_code(state_index_);
}

AreaPicker::Outcome AreaPicker::select() {
    if (level_ == Level::States) {
        if (selected_ >= state_count_) return Outcome::Stay;
        state_index_ = states_[selected_];
        state_row_ = selected_;
        // Major areas only: at least kMajorAreaStations stations, or a state capital.
        area_count_ = 0;
        for (std::size_t i = 0; i < area_count() && area_count_ < kMaxAreas; ++i) {
            const Area &row = area_at(i);
            if (!equal_ci(kStateCodes[state_index_], text(row.state))) continue;
            if (row.count < kMajorAreaStations && !is_capital(i)) continue;
            areas_[area_count_++] = static_cast<uint16_t>(i);
        }
        std::sort(areas_, areas_ + area_count_, picker_area_before);
        level_ = Level::Areas;
        selected_ = 0;
        return Outcome::Stay;
    }
    if (selected_ == area_count_) return Outcome::Auto;
    if (selected_ > area_count_) return Outcome::Stay;
    chosen_ = areas_[selected_];
    return Outcome::Area;
}

AreaPicker::Outcome AreaPicker::back() {
    if (level_ == Level::Areas) {
        level_ = Level::States;
        selected_ = state_row_;
        return Outcome::Stay;
    }
    return Outcome::Close;
}

void TunedUrlCache::clear() {
    for (auto &entry : entries_) {
        entry.used = false;
        entry.station = 0;
        entry.url[0] = '\0';
    }
}

const char *TunedUrlCache::find(uint16_t station) {
    for (std::size_t i = 0; i < kCacheEntries; ++i) {
        if (!entries_[i].used || entries_[i].station != station) continue;
        if (i) {
            // Move the hit to the front, keeping the other entries in recency order.
            const StreamCacheEntry hit = entries_[i];
            std::memmove(&entries_[1], &entries_[0], i * sizeof(StreamCacheEntry));
            entries_[0] = hit;
        }
        return entries_[0].url;
    }
    return nullptr;
}

void TunedUrlCache::store(uint16_t station, const char *url) {
    if (!url) return;
    StreamCacheEntry fresh = {};
    fresh.station = station;
    fresh.used = true;
    std::snprintf(fresh.url, sizeof(fresh.url), "%s", url);
    // An existing entry for this row is overwritten; otherwise the oldest falls out.
    std::size_t slot = kCacheEntries - 1;
    for (std::size_t i = 0; i < kCacheEntries; ++i) {
        if (entries_[i].used && entries_[i].station == station) {
            slot = i;
            break;
        }
    }
    std::memmove(&entries_[1], &entries_[0], slot * sizeof(StreamCacheEntry));
    entries_[0] = fresh;
}

}  // namespace au_radio
