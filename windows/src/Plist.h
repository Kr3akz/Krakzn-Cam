#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Minimaler Leser/Schreiber für XML-Property-Lists, wie sie der
// Apple Mobile Device Service (usbmux) verwendet.
struct PlistValue {
    enum class Type { None, Dict, Array, String, Integer, Real, Bool, Data, Date };

    Type type = Type::None;
    std::string text;  // String, Data (Base64), Date
    int64_t integer = 0;
    double real = 0.0;
    bool boolean = false;
    std::vector<std::string> keys;   // nur Dict
    std::vector<PlistValue> values;  // Dict-Werte bzw. Array-Elemente

    const PlistValue* Find(std::string_view key) const;
    int64_t IntegerOr(std::string_view key, int64_t fallback) const;
    std::string StringOr(std::string_view key, const std::string& fallback) const;
};

bool ParsePlistXml(std::string_view xml, PlistValue& out);

class PlistDictWriter {
public:
    void AddString(std::string_view key, std::string_view value);
    void AddInteger(std::string_view key, int64_t value);
    std::string Finish() const;

private:
    std::string m_body;
};
