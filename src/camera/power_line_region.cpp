#include "camera/power_line.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace ixc::camera {

int MainsHzForRegion(std::string_view iso2) {
    if (iso2.size() != 2) return 0;
    std::string c(iso2);
    for (char& ch : c) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (!std::isalpha(static_cast<unsigned char>(c[0])) || !std::isalpha(static_cast<unsigned char>(c[1]))) return 0;
    if (c == "JP") return 0;  // 50 Hz in the east, 60 Hz in the west
    // 60 Hz countries and territories; everywhere else uses 50 Hz.
    static constexpr const char* k60[] = {"US", "CA", "MX", "GT", "BZ", "SV", "HN", "NI", "CR", "PA", "CU", "DO", "HT", "PR",
                                          "VI", "GU", "AS", "MP", "BS", "BM", "KY", "TT", "AG", "KN", "AW", "CO", "VE", "EC",
                                          "PE", "BR", "GY", "SR", "TW", "KR", "PH", "SA", "LR", "FM", "MH", "PW"};
    return std::any_of(std::begin(k60), std::end(k60), [&](const char* x) { return c == x; }) ? 60 : 50;
}

AntiFlicker ResolveAntiFlicker(AntiFlicker a, std::string_view iso2) {
    if (a != AntiFlicker::Auto) return a;
    switch (MainsHzForRegion(iso2)) {
        case 50: return AntiFlicker::Hz50;
        case 60: return AntiFlicker::Hz60;
        default: return AntiFlicker::Auto;
    }
}

}  // namespace ixc::camera
