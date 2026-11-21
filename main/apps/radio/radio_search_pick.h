#pragma once

#include <cstddef>

// Chooses the stream for a name-based preset from one radio-browser result.
// Used by the Australian presets, whose stream URLs are not hard-coded.
struct RadioSearchBest {
    char url[256];
    long votes;
    int tier;   // 0 MP3, 1 direct AAC, 2 HLS AAC; lower is preferred
    bool found;
};

// Considers one station object from a radio-browser search response. It keeps
// the best qualifying entry: MP3 before direct AAC before HLS AAC, then the
// highest votes within a tier. Entries must be working, have a usable http(s)
// URL, match `wanted` (ASCII case-insensitive) and declare at most 128 kbps
// (unknown bitrate is accepted). Ties keep the earlier entry.
// With `whole_word`, the name must contain `wanted` as a whole word (so "SEN" does not
// match "Sensation") and the entry's countrycode must be AU. Used for a shortened-query
// retry, where a loose name match picked a different station.
// Returns true when `best` changed.
bool radio_search_consider(const char *station_json, const char *wanted, RadioSearchBest *best,
                           bool whole_word = false);
