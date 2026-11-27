#pragma once
#include <map>
#include <string>
#include <string_view>

// Liest ein JSON-Objekt der obersten Ebene (z. B. HELLO). Verschachtelte Werte werden
// übersprungen und als Type::Other abgelegt.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Other };
    Type type = Type::Null;
    double number = 0.0;
    bool boolean = false;
    std::string text;
};

bool ParseJsonObject(std::string_view json, std::map<std::string, JsonValue>& out);
