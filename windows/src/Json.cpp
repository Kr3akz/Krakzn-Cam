#include "Json.h"

#include <cstdlib>

namespace {

void AppendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (code >> 12));
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
                if (i + 4 > s.size()) return false;
                std::string hex(s.substr(i, 4));
                i += 4;
                AppendUtf8(out, static_cast<unsigned>(strtoul(hex.c_str(), nullptr, 16)));
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
        char c = s[i];
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return ParseString(v.text);
        }
        if (c == '{' || c == '[') {
            v.type = JsonValue::Type::Other;
            return SkipContainer(depth);
        }
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
        size_t used = static_cast<size_t>(end - number.c_str());
        if (used == 0) return false;
        i += used;
        v.type = JsonValue::Type::Number;
        return true;
    }

    bool SkipContainer(int depth) {
        char open = s[i];
        char close = open == '{' ? '}' : ']';
        ++i;
        SkipWhitespace();
        if (i < s.size() && s[i] == close) {
            ++i;
            return true;
        }
        while (true) {
            if (open == '{') {
                SkipWhitespace();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWhitespace();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
            }
            JsonValue ignored;
            if (!ParseValue(ignored, depth + 1)) return false;
            SkipWhitespace();
            if (i >= s.size()) return false;
            if (s[i] == ',') {
                ++i;
                continue;
            }
            if (s[i] == close) {
                ++i;
                return true;
            }
            return false;
        }
    }
};

}  // namespace

bool ParseJsonObject(std::string_view json, std::map<std::string, JsonValue>& out) {
    Parser p{json};
    p.SkipWhitespace();
    if (p.i >= json.size() || json[p.i] != '{') return false;
    ++p.i;
    p.SkipWhitespace();
    if (p.i < json.size() && json[p.i] == '}') return true;
    while (true) {
        p.SkipWhitespace();
        std::string key;
        if (!p.ParseString(key)) return false;
        p.SkipWhitespace();
        if (p.i >= json.size() || json[p.i] != ':') return false;
        ++p.i;
        JsonValue value;
        if (!p.ParseValue(value, 1)) return false;
        out[key] = std::move(value);
        p.SkipWhitespace();
        if (p.i >= json.size()) return false;
        if (json[p.i] == ',') {
            ++p.i;
            continue;
        }
        return json[p.i] == '}';
    }
}
