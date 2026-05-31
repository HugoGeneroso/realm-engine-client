#pragma once

struct FeatureCommand {
    char key[64] = {};
    char valueType[8] = {};
    char value[128] = {};

    bool Is(const char* name) const;
    bool Bool() const;
    int Int() const;
    float Float() const;
};

namespace FeatureCommandRegistry {

bool Apply(const FeatureCommand& feature);
int ResolveHotkeyVk(const char* raw);

} // namespace FeatureCommandRegistry
