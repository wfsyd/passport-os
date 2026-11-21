// Table-mode lookups, capacity, and lazy stream resolution against the fixture
// table. The Python driver generates the fixture (AU_RADIO_DATA_INC) and builds this file.
#include "apps/radio/au_radio_table.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace au_radio;

static_assert(sizeof(TableRam) <= kTableRamBudgetBytes);
static_assert(sizeof(TableRam) <= 4192, "table mode must end before the HLS tail (directory layout 4192)");

static unsigned resolves = 0;
static bool resolve_ok = true;
static bool fake_resolve(const char *query, char *url, std::size_t capacity) {
    ++resolves;
    if (!resolve_ok) return false;
    std::snprintf(url, capacity, "http://stream.test/%s.mp3", query);
    return true;
}

static void lookups() {
    // Fixture: Testville (4 rows), Fixtureton (1 row), Bulk (100 FM rows).
    assert(area_count() == 3);
    assert(station_count() == 105);
    assert(std::strcmp(area_name(0), "Testville") == 0);
    assert(std::strcmp(text(station_at(0).name), "Test FM 1") == 0);
    assert(station_at(0).frequency == 1000 && station_at(0).band == kBandFm);
    assert(std::strcmp(band_label(kBandDab), "DAB+") == 0);

    std::size_t area = 99;
    assert(area_by_name("testville", &area) && area == 0);
    assert(!area_by_name("VIC", &area) && !area_by_name("nsw", &area));  // a bare state does not match
    assert(!area_by_name("Nowhere", &area) && !area_by_name("", &area));

    double km = -1;
    assert(nearest_area(-33.8, 151.2, &area, &km) && area == 0 && km < 0.5);
    assert(nearest_area(-37.8, 144.9, &area, &km) && area == 1);
    // A name match wins; coordinates are a fallback within 150 km only.
    assert(area_for_profile("Fixtureton", "", "", "", &area) && area == 1);
    assert(area_for_profile("Unknown", "", "-33.9", "151.1", &area) && area == 0);
    assert(!area_for_profile("Unknown", "", "-30.0", "151.2", &area));
    assert(!area_for_profile("", "", "", "", &area));
    assert(!area_for_profile("", "", "abc", "151.2", &area));
}

static void collection() {
    uint16_t rows[kMaxListed];
    assert(collect_stations(0, kBandsAll, rows, kMaxListed) == 4);
    assert(collect_stations(0, 1u << kBandFm, rows, kMaxListed) == 2);
    assert(rows[0] == 0 && rows[1] == 1);
    assert(collect_stations(0, 1u << kBandDab, rows, kMaxListed) == 1);
    assert(collect_stations(0, kBandsAll, rows, 3) == 3);
    // Capacity: a 100-row area lists at most kMaxListed (96) rows, in table order.
    assert(collect_stations(2, kBandsAll, rows, kMaxListed) == kMaxListed);
    assert(rows[kMaxListed - 1] == 5 + 96 - 1);
    assert(collect_stations(7, kBandsAll, rows, kMaxListed) == 0);
    assert(collect_stations(0, kBandsAll, nullptr, kMaxListed) == 0);
}

static void lazy_resolve() {
    TunedUrlCache cache;
    cache.clear();
    char url[kUrlBytes] = {};
    resolves = 0;
    resolve_ok = true;

    // Success: one resolve, the URL is cached for the row.
    assert(resolve_stream(cache, 0, "Test FM 1", url, sizeof(url), fake_resolve) == StreamLookup::Resolved);
    assert(resolves == 1 && std::strcmp(url, "http://stream.test/Test FM 1.mp3") == 0);

    // Cache hit: no second directory request for the same row.
    url[0] = '\0';
    assert(resolve_stream(cache, 0, "Test FM 1", url, sizeof(url), fake_resolve) == StreamLookup::Cached);
    assert(resolves == 1 && std::strcmp(url, "http://stream.test/Test FM 1.mp3") == 0);

    // Failure: no URL, nothing cached, so the next tune asks again.
    resolve_ok = false;
    url[0] = '\0';
    assert(resolve_stream(cache, 1, "Test FM 2", url, sizeof(url), fake_resolve) == StreamLookup::NotFound);
    assert(resolves == 2 && url[0] == '\0' && cache.find(1) == nullptr);
    resolve_ok = true;
    assert(resolve_stream(cache, 1, "Test FM 2", url, sizeof(url), fake_resolve) == StreamLookup::Resolved);
    assert(resolves == 3);
}

static void cache_capacity() {
    TunedUrlCache cache;
    cache.clear();
    cache.store(10, "http://a/10");
    cache.store(11, "http://a/11");
    cache.store(12, "http://a/12");
    cache.store(13, "http://a/13");
    assert(cache.find(10) != nullptr);          // 10 is now most recent
    cache.store(14, "http://a/14");              // evicts the least recent: 11
    assert(cache.find(11) == nullptr);
    assert(std::strcmp(cache.find(10), "http://a/10") == 0);
    assert(std::strcmp(cache.find(14), "http://a/14") == 0);
    cache.store(12, "http://a/12b");            // an existing row is replaced, not duplicated
    assert(std::strcmp(cache.find(12), "http://a/12b") == 0);
    assert(cache.find(13) != nullptr);
    cache.clear();
    assert(cache.find(10) == nullptr);
}

static void states_and_bands() {
    // Picker states: fixed AU order; areas match the state code case-insensitively.
    assert(std::strcmp(state_code(0), "NSW") == 0 && std::strcmp(state_code(7), "NT") == 0);
    assert(std::strcmp(state_code(kStateCount), "") == 0);
    uint16_t out[8];
    assert(areas_in_state("nsw", out, 8) == 1 && out[0] == 0);
    assert(areas_in_state("VIC", out, 8) == 1 && out[0] == 1);
    assert(areas_in_state("SA", out, 8) == 0);  // no table rows: the picker hides it
    assert(areas_in_state("", out, 8) == 0 && areas_in_state("NSW", out, 0) == 0);

    assert(bands_in_area(0) == 0x7);
    assert(bands_in_area(1) == (1u << kBandFm));
    assert(bands_in_area(9) == 0);
    assert(first_band(0x7) == kBandFm && first_band(1u << kBandDab) == kBandDab && first_band(0) == kBandFm);
    assert(next_band(kBandFm, 0x7) == kBandAm);
    assert(next_band(kBandAm, 0x7) == kBandDab);
    assert(next_band(kBandDab, 0x7) == kBandFm);
    assert(next_band(kBandFm, 1u << kBandFm) == kBandFm);  // only one band: stays
    assert(next_band(kBandAm, (1u << kBandFm) | (1u << kBandDab)) == kBandDab);  // skips a missing band
    assert(next_band(kBandDab, (1u << kBandFm) | (1u << kBandDab)) == kBandFm);
}

static void readouts_and_choices() {
    char text[48];
    // Table order: 0 FM 1000, 1 FM 1010, 2 AM 630, 3 DAB 9A (Testville).
    format_dial_readout(station_at(0), text, sizeof(text));
    assert(std::strcmp(text, "100.0") == 0);
    format_dial_readout(station_at(2), text, sizeof(text));
    assert(std::strcmp(text, "630 kHz") == 0);
    format_dial_readout(station_at(3), text, sizeof(text));
    assert(std::strcmp(text, "9A") == 0);
    const Station simulcast{0, 0, 0, 0, kBandDab};  // empty dab_block: pool offset 0 is ""
    format_dial_readout(simulcast, text, sizeof(text));
    assert(std::strcmp(text, "DAB+") == 0);

    assert(format_area_choice(0, text, sizeof(text)) && std::strcmp(text, "Testville, NSW") == 0);
    assert(!format_area_choice(9, text, sizeof(text)));
    std::size_t area = 99;
    assert(area_for_choice("Testville, NSW", &area) && area == 0);
    assert(area_for_choice("testville,nsw", &area) && area == 0);
    assert(area_for_choice("Fixtureton", &area) && area == 1);
    assert(!area_for_choice("Testville, VIC", &area));  // a stale state does not match
    assert(!area_for_choice(", NSW", &area) && !area_for_choice("", &area) && !area_for_choice(nullptr, &area));

    // Selection order: picked area, then profile city, then coordinates.
    assert(area_for_selection("Fixtureton, VIC", "Testville", "", "-33.8", "151.2", &area) && area == 1);
    assert(area_for_selection("", "", "", "-33.9", "151.1", &area) && area == 0);
    assert(area_for_selection("", "Fixtureton", "", "", "", &area) && area == 1);
    assert(!area_for_selection("Testville, VIC", "", "", "", "", &area));
}

static void picker_flow() {
    using Outcome = AreaPicker::Outcome;
    AreaPicker picker;
    picker.open();
    // Only states with rows are listed, in AU order.
    assert(picker.level() == AreaPicker::Level::States && picker.count() == 3);
    assert(std::strcmp(picker.label(0), "NSW") == 0 && std::strcmp(picker.label(2), "QLD") == 0);
    assert(std::strcmp(picker.title(), "Area") == 0);
    picker.move(-3);
    assert(picker.selected() == 0);
    picker.move(5);
    assert(picker.selected() == 2);
    picker.move(-1);
    assert(picker.selected() == 1);

    // VIC: Fixtureton has 1 row and is not a capital, so only Auto is listed (last row).
    assert(picker.select() == Outcome::Stay && picker.count() == 1);
    assert(picker.level() == AreaPicker::Level::Areas && std::strcmp(picker.title(), "VIC") == 0);
    assert(std::strcmp(picker.label(0), AreaPicker::kAutoLabel) == 0);
    assert(picker.select() == Outcome::Auto);

    // Long OK goes back one level and keeps the state row highlighted.
    assert(picker.back() == Outcome::Stay && picker.level() == AreaPicker::Level::States);
    assert(picker.selected() == 1);

    // QLD: Bulk has 100 rows, so it is listed first, then Auto.
    picker.move(1);
    assert(picker.select() == Outcome::Stay && picker.count() == 2);
    assert(std::strcmp(picker.label(0), "Bulk") == 0);
    assert(std::strcmp(picker.label(1), AreaPicker::kAutoLabel) == 0);
    assert(picker.select() == Outcome::Area && picker.chosen_area() == 2);
    assert(picker.back() == Outcome::Stay && picker.back() == Outcome::Close);
}

int main() {
    lookups();
    collection();
    lazy_resolve();
    cache_capacity();
    states_and_bands();
    readouts_and_choices();
    picker_flow();
    std::puts("AU radio table: lookups, nearest/profile selection, capacity, lazy resolve, LRU cache, "
              "states, bands, readouts, area choice, picker: PASS");
    return 0;
}
