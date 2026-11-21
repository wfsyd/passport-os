#pragma once

#include "radio_player.h"

#include <cstddef>

// Country used when the location is unknown (lookup failed or no profile country).
// Set by the build option PASSPORT_DEFAULT_COUNTRY (main/CMakeLists.txt); "AU" unless overridden.
#ifndef PASSPORT_DEFAULT_COUNTRY
#define PASSPORT_DEFAULT_COUNTRY "AU"
#endif

struct RadioLocation {
    char country_code[4];
    char region[48];
    char city[48];
    char search_city[48];
};

using RadioCatalogCancelled = bool (*)();

// Locates the radio from its public network address and loads playable MP3
// stations for the detected city. If the directory has no city result, it
// automatically falls back to the surrounding region/province.
std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                   RadioLocation *location,
                                   RadioCatalogCancelled cancelled = nullptr);

// True for a Chinese city or province name (Chinese or pinyin, optional 市/县)
// the directory search understands; used to validate voice requests.
bool radio_catalog_known_place(const char *name);

// Loads the most popular playable stations for a user-selected Chinese city.
// This skips IP geolocation and is used by the on-device city selector.
std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                        std::size_t capacity,
                                        RadioLocation *location);

// Preferred profile location skips IP geolocation; empty city uses IP instead.
// Sparse local results are topped up with province results and curated presets.
// Voice requests may pass station keywords (including "" for any local station):
// stop directory lookups once that station is found, then add presets locally.
std::size_t radio_catalog_discover_location(const RadioLocation &preferred,
                                           RadioStation *stations, std::size_t capacity,
                                           RadioLocation *location,
                                           RadioCatalogCancelled cancelled = nullptr,
                                           const char *voice_station = nullptr);

// Table mode: resolves one stream URL by name through the AU directory. `query` is a
// substring of the directory station name. Returns false when no playable entry
// matches or `cancelled` reports the request as stale.
bool radio_catalog_resolve_stream(const char *query, char *url, std::size_t capacity,
                                  RadioCatalogCancelled cancelled = nullptr);
