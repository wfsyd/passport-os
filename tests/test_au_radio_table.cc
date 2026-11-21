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
    assert(area_by_name("FX", &area) && area == 1);
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

int main() {
    lookups();
    collection();
    lazy_resolve();
    cache_capacity();
    std::puts("AU radio table: lookups, nearest/profile selection, capacity, lazy resolve, LRU cache: PASS");
    return 0;
}
