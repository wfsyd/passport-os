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
constexpr double kPreferKm = 25.0;
// Areas with at least this many stations are listed in the area picker (capitals always are).
constexpr std::size_t kMajorAreaStations = 8;
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

// Case-insensitive match on the area name only. A state code does not match, so a bare
// state never picks an arbitrary area of that state.
bool area_by_name(const char *name, std::size_t *area);
// Nearest area by great-circle distance; km receives the distance. Among the areas within
// kPreferKm, the one with the most stations wins (the metro area, not a small suburb
// beside it). Without any such area, this is the plain nearest area.
bool nearest_area(double lat, double lon, std::size_t *area, double *km);
// Area for a profile: city name, then region name (only if it is an area name), then the
// GPS point via nearest_area within kMaxSelectKm.
bool area_for_profile(const char *city, const char *region, const char *lat, const char *lon,
                      std::size_t *area);
// Row indices of the area's stations whose band is in band_mask, in table order.
std::size_t collect_stations(std::size_t area, uint8_t band_mask, uint16_t *out, std::size_t capacity);

// Fixed AU state order used by the Area picker (NSW first, then the rest).
constexpr std::size_t kStateCount = 8;
const char *state_code(std::size_t index);
// Areas whose state abbreviation matches `state` (case-insensitive), in table order.
std::size_t areas_in_state(const char *state, uint16_t *out, std::size_t capacity);
// Bit mask of the bands (1 << band) that have at least one row in `area`.
uint8_t bands_in_area(std::size_t area);
// Band to show first: FM when present, else the lowest band in the mask (0 for an empty mask).
uint8_t first_band(uint8_t band_mask);
// Next band after `current` (FM, AM, DAB+, then FM again) that is in the mask.
// Returns `current` when no other band is available.
uint8_t next_band(uint8_t current, uint8_t band_mask);
// Dial readout for a row: "94.7" (FM), "774 kHz" (AM) or the DAB block such as "9C".
void format_dial_readout(const Station &row, char *out, std::size_t capacity);
// The value saved in the profile's radio_city for a picked area: "Name, ST".
bool format_area_choice(std::size_t area, char *out, std::size_t capacity);
// Area for a saved choice. Accepts "Name, ST" or a bare "Name".
bool area_for_choice(const char *choice, std::size_t *area);
// User-picked choice first, then the profile city/region/coordinates (area_for_profile).
bool area_for_selection(const char *choice, const char *city, const char *region, const char *lat,
                        const char *lon, std::size_t *area);

// The rule that chose an area, for logs. None when no rule matched.
enum class AreaRule : uint8_t { None, Choice, City, Region, Gps };
const char *area_rule_name(AreaRule rule);
// As area_for_selection, also reporting the rule and, for Gps, the distance in km.
// rule and km may be null.
bool area_for_selection_detail(const char *choice, const char *city, const char *region,
                               const char *lat, const char *lon, std::size_t *area,
                               AreaRule *rule, double *km);

// Stream-query fallback for a "No stream" miss: drops trailing frequency or band tokens
// ("KIIS 106.5" -> "KIIS", "ABC Radio 774 AM" -> "ABC Radio"). False when nothing would
// be dropped, when the result would be empty, or when `out` is too small.
bool shorter_query(const char *query, char *out, std::size_t capacity);

// Area picker state machine (states, then areas of the chosen state). Pure logic: the
// UI renders label() rows and feeds it up/down/OK/long-OK. At the areas level the rows are
// the major areas of the state (the capital first, then A-Z), and the last row is Auto.
class AreaPicker {
public:
    static constexpr std::size_t kMaxAreas = 48;
    static constexpr const char *kAutoLabel = "Auto (use my location)";
    enum class Level : uint8_t { States, Areas };
    enum class Outcome : uint8_t { Stay, Auto, Area, Close };

    void open();
    Level level() const { return level_; }
    // Title for the page: "Area" at the state level, the state code inside a state.
    const char *title() const;
    std::size_t count() const;
    std::size_t selected() const { return selected_; }
    void move(int delta);
    // Stable text for row i: a state code, a table area name, or kAutoLabel (last row of areas).
    const char *label(std::size_t i) const;
    Outcome select();  // OK: enter a state, or choose the highlighted area
    Outcome back();    // long OK: areas -> states, states -> Close
    std::size_t chosen_area() const { return chosen_; }

private:
    Level level_ = Level::States;
    std::size_t selected_ = 0;
    std::size_t state_row_ = 0;
    std::size_t chosen_ = 0;
    uint8_t states_[kStateCount] = {};
    std::size_t state_count_ = 0;
    uint16_t areas_[kMaxAreas] = {};
    std::size_t area_count_ = 0;
    uint8_t state_index_ = 0;
};

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
    uint8_t band;       // the band whose rows `listed` holds (kBandFm/kBandAm/kBandDab)
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
