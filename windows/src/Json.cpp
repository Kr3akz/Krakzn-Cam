#include "Json.h"

#include <cstdlib>

namespace {

void AppendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

struct Parser {
    std::string_view s;
    size_t i = 0;

    void SkipWhitespace() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }

    bool Literal(std::string_view literal) {
        if (s.substr(i, literal.size()) == literal) {
            i += literal.size();
            return true;
        }
        return false;
    }

    bool Hex4(unsigned& value) {
        if (i + 4 > s.size()) return false;
        const std::string hex(s.substr(i, 4));
        i += 4;
        char* end = nullptr;
        value = static_cast<unsigned>(strtoul(hex.c_str(), &end, 16));
        return end == hex.c_str() + 4;
    }

    bool ParseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            char escape = s[i++];
            switch (escape) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = 0;
                if (!Hex4(code)) return false;
                // Ersatzpaare (z. B. Emojis in Release-Notizen) zusammensetzen.
                if (code >= 0xD800 && code < 0xDC00 && s.substr(i, 2) == "\\u") {
                    i += 2;
                    unsigned low = 0;
                    if (!Hex4(low)) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool ParseValue(JsonValue& v, int depth) {
        SkipWhitespace();
        if (i >= s.size() || depth > 32) return false;
        const char c = s[i];
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return ParseString(v.text);
        }
        if (c == '{') return ParseObject(v, depth);
        if (c == '[') return ParseArray(v, depth);
        if (Literal("true")) {
            v.type = JsonValue::Type::Bool;
            v.boolean = true;
            return true;
        }
        if (Literal("false")) {
            v.type = JsonValue::Type::Bool;
            v.boolean = false;
            return true;
        }
        if (Literal("null")) {
            v.type = JsonValue::Type::Null;
            return true;
        }
        std::string number(s.substr(i, s.size() - i < 64 ? s.size() - i : 64));
        char* end = nullptr;
        v.number = strtod(number.c_str(), &end);
        const size_t used = static_cast<size_t>(end - number.c_str());
        if (used == 0) return false;
        i += used;
        v.type = JsonValue::Type::Number;
        return true;
    }

    bool ParseObject(JsonValue& v, int depth) {
        v.type = JsonValue::Type::Object;
        ++i;  // {
        SkipWhitespace();
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        while (true) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            JsonValue item;
            if (!ParseValue(item, depth + 1)) return false;
            v.keys.push_back(std::move(key));
            v.items.push_back(std::move(item));
            SkipWhitespace();
            if (i >= s.size()) return false;
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == '}') {
                ++i;
                return true;
            }
            return false;
        }
    }

    bool ParseArray(JsonValue& v, int depth) {
        v.type = JsonValue::Type::Array;
        ++i;  // [
        SkipWhitespace();
        if (i < s.size() && s[i] == ']') {
            ++i;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!ParseValue(item, depth + 1)) return false;
            v.items.push_back(std::move(item));
            SkipWhitespace();
            if (i >= s.size()) return false;
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == ']') {
                ++i;
                return true;
            }
            return false;
        }
    }
};

}  // namespace

const JsonValue* JsonValue::Find(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

std::string JsonValue::String(std::string_view key) const {
    const JsonValue* value = Find(key);
    return value && value->type == Type::String ? value->text : std::string();
}

double JsonValue::Number(std::string_view key, double fallback) const {
    const JsonValue* value = Find(key);
    return value && value->type == Type::Number ? value->number : fallback;
}

bool JsonValue::Bool(std::string_view key, bool fallback) const {
    const JsonValue* value = Find(key);
    return value && value->type == Type::Bool ? value->boolean : fallback;
}

bool ParseJson(std::string_view json, JsonValue& out) {
    Parser p{json};
    if (!p.ParseValue(out, 0)) return false;
    p.SkipWhitespace();
    return p.i == json.size();
}

bool ParseJsonObject(std::string_view json, std::map<std::string, JsonValue>& out) {
    JsonValue root;
    Parser p{json};
    p.SkipWhitespace();
    if (p.i >= json.size() || json[p.i] != '{') return false;
    if (!p.ParseValue(root, 0)) return false;
    for (size_t i = 0; i < root.keys.size(); ++i) out[root.keys[i]] = std::move(root.items[i]);
    return true;
}
