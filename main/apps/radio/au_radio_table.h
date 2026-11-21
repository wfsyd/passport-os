#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Bundled Australian station table (flash). Rows are generated from
// third_party/au-radio/stations.csv by tools/generate_radio_table.py. Table mode
// keeps only row indices in RAM and resolves a row's stream URL when it is tuned.
namespace au_radio {

constexpr uint8_t kBandFm = 0;
constexpr uint8_t kBandAm = 1;
constexpr uint8_t kBandDab = 2;
constexpr uint8_t kBandsAll = 0x7;
constexpr std::size_t kMaxListed = 96;
constexpr std::size_t kCacheEntries = 4;
constexpr std::size_t kUrlBytes = 256;
constexpr double kMaxSelectKm = 150.0;
// Station workspace left for table mode: kStationBytes (10240) minus
// RadioSource::kHlsWorkspaceBytes (6000). Checked against the real layout in radio_player.cc.
constexpr std::size_t kTableRamBudgetBytes = 4240;

struct Area {
    int32_t lat_e4;     // latitude in 1e-4 degrees
    int32_t lon_e4;     // longitude in 1e-4 degrees
    uint16_t id;        // pool offsets from here on
    uint16_t name;
    uint16_t state;
    uint16_t first;     // index of the first station row
    uint16_t count;
};

struct Station {
    uint16_t name;
    uint16_t query;
    uint16_t dab_block;
    uint16_t frequency; // FM in 0.1 MHz, AM in kHz, DAB 0
    uint8_t band;
};

std::size_t area_count();
std::size_t station_count();
const Area &area_at(std::size_t index);
const Station &station_at(std::size_t index);
const char *text(uint16_t offset);
const char *area_name(std::size_t area);
const char *band_label(uint8_t band);

// Case-insensitive match on the area name or the state abbreviation.
bool area_by_name(const char *name, std::size_t *area);
// Nearest area by great-circle distance; km receives the distance.
bool nearest_area(double lat, double lon, std::size_t *area, double *km);
// Area for a profile: city or region name first, then the nearest area within kMaxSelectKm.
bool area_for_profile(const char *city, const char *region, const char *lat, const char *lon,
                      std::size_t *area);
// Row indices of the area's stations whose band is in band_mask, in table order.
std::size_t collect_stations(std::size_t area, uint8_t band_mask, uint16_t *out, std::size_t capacity);

// Most-recently-used stream URLs for table rows, keyed by row index.
struct StreamCacheEntry {
    uint16_t station;
    bool used;
    char url[kUrlBytes];
};

class TunedUrlCache {
public:
    void clear();
    // Returns the cached URL and makes the row the most recent, or nullptr.
    const char *find(uint16_t station);
    void store(uint16_t station, const char *url);

private:
    StreamCacheEntry entries_[kCacheEntries];
};

// Layout of the station workspace in table mode (the directory list is not used).
struct TableRam {
    uint16_t area;
    uint16_t count;
    uint16_t listed[kMaxListed];
    TunedUrlCache cache;
};
static_assert(sizeof(TableRam) <= kTableRamBudgetBytes, "table mode exceeds its RAM budget");

enum class StreamLookup { Cached, Resolved, NotFound };

// Returns the stream URL for `station` (a row index). A cache hit copies the URL
// without calling `resolve`. Otherwise `resolve(query, url, capacity)` runs once and
// its result is cached. A failed resolve is not cached, so a later tune retries.
template <typename Resolve>
StreamLookup resolve_stream(TunedUrlCache &cache, uint16_t station, const char *query,
                            char *url, std::size_t capacity, Resolve &&resolve) {
    if (const char *cached = cache.find(station)) {
        std::snprintf(url, capacity, "%s", cached);
        return StreamLookup::Cached;
    }
    if (!resolve(query, url, capacity) || url[0] == '\0') return StreamLookup::NotFound;
    cache.store(station, url);
    return StreamLookup::Resolved;
}

}  // namespace au_radio
