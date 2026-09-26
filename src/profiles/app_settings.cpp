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
    return s;
}

std::string AppSettingsToJson(const AppSettings& s) {
    return json::Serialize(json::Object{{"activeProfile", s.activeProfile}, {"hotkeysEnabled", s.hotkeysEnabled}});
}

}  // namespace ixc
