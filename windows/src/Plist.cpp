#include "Plist.h"

#include <cstdlib>

namespace {

std::string Unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semicolon = s.find(';', i);
        if (semicolon == std::string_view::npos) {
            out += s[i];
            continue;
        }
        std::string_view entity = s.substr(i + 1, semicolon - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else out.append(s.substr(i, semicolon - i + 1));
        i = semicolon;
    }
    return out;
}

std::string Escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c; break;
        }
    }
    return out;
}

struct Tag {
    std::string name;
    bool closing = false;
    bool selfClosing = false;
};

class Cursor {
public:
    explicit Cursor(std::string_view s) : m_s(s) {}

    // Nächstes Element-Tag; überspringt <?xml ...?> und <!DOCTYPE ...>.
    bool NextTag(Tag& tag) {
        while (true) {
            size_t lt = m_s.find('<', m_pos);
            if (lt == std::string_view::npos) return false;
            size_t gt = m_s.find('>', lt);
            if (gt == std::string_view::npos) return false;
            m_pos = gt + 1;
            char first = lt + 1 < m_s.size() ? m_s[lt + 1] : '\0';
            if (first == '?' || first == '!') continue;

            std::string_view inner = m_s.substr(lt + 1, gt - lt - 1);
            tag.closing = !inner.empty() && inner.front() == '/';
            if (tag.closing) inner.remove_prefix(1);
            tag.selfClosing = !inner.empty() && inner.back() == '/';
            if (tag.selfClosing) inner.remove_suffix(1);
            size_t end = inner.find_first_of(" \t\r\n");
            tag.name = std::string(inner.substr(0, end));
            return true;
        }
    }

    std::string_view TextUntilTag() {
        size_t lt = m_s.find('<', m_pos);
        if (lt == std::string_view::npos) lt = m_s.size();
        std::string_view text = m_s.substr(m_pos, lt - m_pos);
        m_pos = lt;
        return text;
    }

private:
    std::string_view m_s;
    size_t m_pos = 0;
};

bool ParseValue(Cursor& c, const Tag& open, PlistValue& v, int depth);

bool ParseNext(Cursor& c, PlistValue& v, int depth) {
    Tag tag;
    if (!c.NextTag(tag) || tag.closing) return false;
    return ParseValue(c, tag, v, depth);
}

bool ReadScalar(Cursor& c, const Tag& open, std::string& text) {
    if (open.selfClosing) {
        text.clear();
        return true;
    }
    text = Unescape(c.TextUntilTag());
    Tag close;
    return c.NextTag(close) && close.closing && close.name == open.name;
}

bool ParseValue(Cursor& c, const Tag& open, PlistValue& v, int depth) {
    if (depth > 32) return false;
    const std::string& name = open.name;

    if (name == "dict") {
        v.type = PlistValue::Type::Dict;
        if (open.selfClosing) return true;
        while (true) {
            Tag tag;
            if (!c.NextTag(tag)) return false;
            if (tag.closing && tag.name == "dict") return true;
            if (tag.closing || tag.name != "key") return false;
            std::string key;
            if (!ReadScalar(c, tag, key)) return false;
            PlistValue child;
            if (!ParseNext(c, child, depth + 1)) return false;
            v.keys.push_back(std::move(key));
            v.values.push_back(std::move(child));
        }
    }

    if (name == "array") {
        v.type = PlistValue::Type::Array;
        if (open.selfClosing) return true;
        while (true) {
            Tag tag;
            if (!c.NextTag(tag)) return false;
            if (tag.closing && tag.name == "array") return true;
            if (tag.closing) return false;
            PlistValue child;
            if (!ParseValue(c, tag, child, depth + 1)) return false;
            v.values.push_back(std::move(child));
        }
    }

    if (name == "true" || name == "false") {
        v.type = PlistValue::Type::Bool;
        v.boolean = name == "true";
        if (open.selfClosing) return true;
        Tag close;
        return c.NextTag(close) && close.closing;
    }

    std::string text;
    if (!ReadScalar(c, open, text)) return false;
    if (name == "string") {
        v.type = PlistValue::Type::String;
        v.text = std::move(text);
    } else if (name == "integer") {
        v.type = PlistValue::Type::Integer;
        v.integer = _strtoi64(text.c_str(), nullptr, 10);
    } else if (name == "real") {
        v.type = PlistValue::Type::Real;
        v.real = strtod(text.c_str(), nullptr);
    } else if (name == "data") {
        v.type = PlistValue::Type::Data;
        v.text = std::move(text);
    } else if (name == "date") {
        v.type = PlistValue::Type::Date;
        v.text = std::move(text);
    } else {
        return false;
    }
    return true;
}

}  // namespace

const PlistValue* PlistValue::Find(std::string_view key) const {
    if (type != Type::Dict) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &values[i];
    }
    return nullptr;
}

int64_t PlistValue::IntegerOr(std::string_view key, int64_t fallback) const {
    const PlistValue* v = Find(key);
    return v && v->type == Type::Integer ? v->integer : fallback;
}

std::string PlistValue::StringOr(std::string_view key, const std::string& fallback) const {
    const PlistValue* v = Find(key);
    return v && v->type == Type::String ? v->text : fallback;
}

bool ParsePlistXml(std::string_view xml, PlistValue& out) {
    Cursor c(xml);
    Tag tag;
    while (c.NextTag(tag)) {
        if (!tag.closing && tag.name == "plist") return ParseNext(c, out, 0);
    }
    return false;
}

void PlistDictWriter::AddString(std::string_view key, std::string_view value) {
    m_body += "\t<key>" + Escape(key) + "</key>\n\t<string>" + Escape(value) + "</string>\n";
}

void PlistDictWriter::AddInteger(std::string_view key, int64_t value) {
    m_body += "\t<key>" + Escape(key) + "</key>\n\t<integer>" + std::to_string(value) + "</integer>\n";
}

std::string PlistDictWriter::Finish() const {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n" +
           m_body + "</dict>\n</plist>\n";
}
