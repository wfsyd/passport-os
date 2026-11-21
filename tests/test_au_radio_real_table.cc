// Checks the bundled Australian station table (main/apps/radio/au_radio_table_data.inc):
// GPS and name selection for Sydney, the area picker's order for each state, and that no
// DAB+-only area sits within 70 km of an FM/AM area. Built by tests/test_au_radio_table.py.
#include "apps/radio/au_radio_table.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace au_radio;

static bool has_station(std::size_t area, uint8_t band, const char *name, const char *block) {
    uint16_t rows[kMaxListed];
    const std::size_t n = collect_stations(area, 1u << band, rows, kMaxListed);
    for (std::size_t i = 0; i < n; ++i) {
        const Station &row = station_at(rows[i]);
        if (std::strcmp(text(row.name), name) == 0 && (!block || std::strcmp(text(row.dab_block), block) == 0))
            return true;
    }
    return false;
}

static void sydney() {
    std::size_t area = 99;
    double km = -1;
    // GPS in Sydney picks the Sydney area (the dense one), not the nearby small areas.
    assert(area_for_profile("", "", "-33.87", "151.21", &area));
    assert(std::strcmp(area_name(area), "Sydney") == 0 && std::strcmp(text(area_at(area).state), "NSW") == 0);
    assert(nearest_area(-33.87, 151.21, &area, &km) && std::strcmp(area_name(area), "Sydney") == 0 && km < 25);
    assert(area_count() > 0 && area_at(area).count == 40);

    // FM first, with 12 FM rows, then AM and DAB+.
    assert(first_band(bands_in_area(area)) == kBandFm);
    uint16_t rows[kMaxListed];
    assert(collect_stations(area, 1u << kBandFm, rows, kMaxListed) == 12);
    assert(collect_stations(area, 1u << kBandAm, rows, kMaxListed) == 9);
    assert(collect_stations(area, 1u << kBandDab, rows, kMaxListed) == 19);
    assert(has_station(area, kBandDab, "ABC Jazz", "9C"));
    assert(has_station(area, kBandDab, "Double J", "9C"));
    assert(has_station(area, kBandDab, "SBS PopAsia", "9C"));

    // A city name wins over coordinates, and a bare state never picks an area.
    assert(area_for_profile("Sydney", "", "", "", &area) && std::strcmp(area_name(area), "Sydney") == 0);
    assert(area_for_profile("Parramatta", "NSW", "-33.87", "151.21", &area));
    assert(std::strcmp(area_name(area), "Sydney") == 0);
    assert(!area_for_profile("", "NSW", "", "", &area));
    assert(!area_by_name("Waverley", &area));  // the DAB+-only site folded into its neighbours
}

static bool label_before(const char *a, const char *b) {
    for (;; ++a, ++b) {
        const int ca = std::tolower(static_cast<unsigned char>(*a));
        const int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb) return ca < cb;
        if (ca == '\0') return false;
    }
}

static void picker_order() {
    using Outcome = AreaPicker::Outcome;
    AreaPicker picker;
    picker.open();
    const std::size_t states = picker.count();
    assert(states >= 1 && std::strcmp(picker.label(0), "NSW") == 0);
    for (std::size_t s = 0; s < states; ++s) {
        picker.move(s == 0 ? -10 : 1);  // state row s (after back() the state row stays highlighted)
        assert(picker.selected() == s);
        assert(picker.select() == Outcome::Stay && picker.level() == AreaPicker::Level::Areas);
        const std::size_t rows = picker.count();
        assert(rows >= 2 && rows - 1 <= AreaPicker::kMaxAreas);
        // Last row is Auto. The rows above it are major areas: the capital first, then A-Z.
        assert(std::strcmp(picker.label(rows - 1), AreaPicker::kAutoLabel) == 0);
        for (std::size_t i = 1; i + 2 < rows; ++i) assert(label_before(picker.label(i), picker.label(i + 1)));
        if (std::strcmp(picker.title(), "NSW") == 0) {
            assert(rows == 3);
            assert(std::strcmp(picker.label(0), "Sydney") == 0);
            assert(std::strcmp(picker.label(1), "Newcastle") == 0);
        }
        assert(picker.back() == Outcome::Stay && picker.level() == AreaPicker::Level::States);
    }
}

static double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    const double rad = 3.14159265358979323846 / 180.0;
    const double dphi = (lat2 - lat1) * rad;
    const double dlam = (lon2 - lon1) * rad;
    const double h = std::sin(dphi / 2) * std::sin(dphi / 2) +
                     std::cos(lat1 * rad) * std::cos(lat2 * rad) * std::sin(dlam / 2) * std::sin(dlam / 2);
    return 2 * 6371.0 * std::asin(std::sqrt(h));
}

static void no_dab_only_areas() {
    // A DAB+-only area is folded into an FM/AM area within 70 km. Any that remain have none in range.
    for (std::size_t i = 0; i < area_count(); ++i) {
        if (bands_in_area(i) != (1u << kBandDab)) continue;
        for (std::size_t j = 0; j < area_count(); ++j) {
            if (bands_in_area(j) == (1u << kBandDab)) continue;
            const double km = haversine_km(area_at(i).lat_e4 / 1e4, area_at(i).lon_e4 / 1e4,
                                           area_at(j).lat_e4 / 1e4, area_at(j).lon_e4 / 1e4);
            assert(km > 70.0 && "a DAB+-only area should have been folded into its neighbour");
        }
    }
}

int main() {
    sydney();
    picker_order();
    no_dab_only_areas();
    std::puts("AU radio real table: Sydney GPS and names, picker order, DAB+-only folding: PASS");
    return 0;
}
