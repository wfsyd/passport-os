#pragma once

#include <cstddef>

// Chooses the stream for a name-based preset from one radio-browser result.
// Used by the Australian presets, whose stream URLs are not hard-coded.
struct RadioSearchBest {
    char url[256];
    long votes;
    bool found;
};

// Considers one station object from a radio-browser search response. It keeps
// the highest-voted entry that is a working, non-HLS MP3 stream and whose name
// contains `wanted` (ASCII case-insensitive). Ties keep the earlier entry.
// Returns true when `best` changed.
bool radio_search_consider(const char *station_json, const char *wanted, RadioSearchBest *best);
