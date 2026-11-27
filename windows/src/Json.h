#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Kleiner JSON-Leser. ParseJsonObject liest nur die oberste Ebene (HELLO, STATE),
// ParseJson auch verschachtelte Objekte und Listen (z. B. die Release-Antwort von GitHub).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Object, Array };
    Type type = Type::Null;
    double number = 0.0;
    bool boolean = false;
    std::string text;
    // Object: keys[i] gehört zu items[i]. Array: nur items.
    std::vector<std::string> keys;
    std::vector<JsonValue> items;

    const JsonValue* Find(std::string_view key) const;
    std::string String(std::string_view key) const;  // leer, wenn nicht vorhanden
    double Number(std::string_view key, double fallback = 0.0) const;
    bool Bool(std::string_view key, bool fallback = false) const;
};

bool ParseJsonObject(std::string_view json, std::map<std::string, JsonValue>& out);
bool ParseJson(std::string_view json, JsonValue& out);
