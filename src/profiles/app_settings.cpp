#include "profiles/app_settings.h"

#include "common/json.h"
#include "profiles/profile_store.h"

namespace ixc {

AppSettings AppSettingsFromJson(std::string_view text) {
    AppSettings s;
    const json::ParseResult r = json::Parse(text);
    if (!r || !r.value->IsObject()) return s;
    if (const json::Value* v = r.value->Find("activeProfile"); v && v->IsString() && ProfileStore::IsValidStem(v->AsString())) {
        s.activeProfile = v->AsString();
    }
    if (const json::Value* v = r.value->Find("hotkeysEnabled"); v && v->IsBool()) s.hotkeysEnabled = v->AsBool();
    if (const json::Value* v = r.value->Find("showFaceMarkers"); v && v->IsBool()) s.showFaceMarkers = v->AsBool();
    if (const json::Value* v = r.value->Find("lastUpdateCheck"); v && v->IsNumber() && v->AsNumber() >= 0) s.lastUpdateCheck = v->AsNumber();
    if (const json::Value* v = r.value->Find("skippedVersion"); v && v->IsString() && v->AsString().size() <= 32) s.skippedVersion = v->AsString();
    return s;
}

std::string AppSettingsToJson(const AppSettings& s) {
    return json::Serialize(json::Object{{"activeProfile", s.activeProfile},
                                        {"hotkeysEnabled", s.hotkeysEnabled},
                                        {"showFaceMarkers", s.showFaceMarkers},
                                        {"lastUpdateCheck", s.lastUpdateCheck},
                                        {"skippedVersion", s.skippedVersion}});
}

}  // namespace ixc
