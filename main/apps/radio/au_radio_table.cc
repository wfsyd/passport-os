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
        const Area &row = area_at(i);
        if (equal_ci(name, text(row.name)) || equal_ci(name, text(row.state))) {
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
    for (std::size_t i = 0; i < area_count(); ++i) {
        const Area &row = area_at(i);
        const double d = distance_km(lat, lon, row.lat_e4 / 1e4, row.lon_e4 / 1e4);
        if (!found || d < best) {
            found = true;
            best = d;
            *area = i;
        }
    }
    if (found && km) *km = best;
    return found;
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
