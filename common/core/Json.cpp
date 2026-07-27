// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Json.h"

#include <cmath>
#include <cstdlib>

#include "platform/Encoding.h"

namespace rabbitears {
namespace {

// One shared null, returned by every failed lookup. Making misses return a real value
// instead of throwing is what lets `root["info"]["name"].asString()` be written without a
// guard at each step — and on these panels a missing field is the normal case, not an error.
const JsonValue& nullValue() {
    static const JsonValue kNull;
    return kNull;
}

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Does this text parse ENTIRELY as a JSON number? Used by the tolerant scalar accessors to
// decide whether a string is really a number. Deliberately stricter than strtod: it must
// consume the whole string, so "8.8 / 10" and "12 Mbps" are NOT numbers, while " 7.5 " is
// (panels pad values). Accepts the JSON grammar plus a leading '+', which some panels emit.
bool numericText(const std::string& s, double* out) {
    size_t i = 0, n = s.size();
    while (i < n && isWs(s[i])) ++i;
    size_t end = n;
    while (end > i && isWs(s[end - 1])) --end;
    if (i >= end) return false;

    const size_t start = i;
    if (s[i] == '+' || s[i] == '-') ++i;
    bool digits = false;
    while (i < end && s[i] >= '0' && s[i] <= '9') { ++i; digits = true; }
    if (i < end && s[i] == '.') {
        ++i;
        while (i < end && s[i] >= '0' && s[i] <= '9') { ++i; digits = true; }
    }
    if (!digits) return false;
    if (i < end && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < end && (s[i] == '+' || s[i] == '-')) ++i;
        bool expDigits = false;
        while (i < end && s[i] >= '0' && s[i] <= '9') { ++i; expDigits = true; }
        if (!expDigits) return false;
    }
    if (i != end) return false;
    if (out) *out = std::strtod(s.c_str() + start, nullptr);
    return true;
}

// Parse an integer out of `s` without going through a double. A stream_id or an exp_date is
// an exact identity: 64-bit values above 2^53 do not survive a double round-trip, and an id
// that comes back off by one silently plays the wrong film.
bool integerText(const std::string& s, long long* out) {
    size_t i = 0, n = s.size();
    while (i < n && isWs(s[i])) ++i;
    size_t end = n;
    while (end > i && isWs(s[end - 1])) --end;
    if (i >= end) return false;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; }
    if (i >= end) return false;
    unsigned long long v = 0;
    for (; i < end; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        const unsigned d = static_cast<unsigned>(s[i] - '0');
        // Saturate rather than wrap. A wrapped id is a plausible-looking wrong number;
        // a saturated one is obviously wrong wherever it surfaces.
        if (v > (0x7FFFFFFFFFFFFFFFull - d) / 10ull) {
            v = 0x7FFFFFFFFFFFFFFFull;
            break;
        }
        v = v * 10ull + d;
    }
    if (out) *out = neg ? -static_cast<long long>(v) : static_cast<long long>(v);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------

size_t JsonValue::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const JsonValue& JsonValue::operator[](size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return nullValue();
    return arr_[i];
}

const JsonValue& JsonValue::operator[](const char* key) const {
    if (type_ != Type::Object || !key) return nullValue();
    for (const auto& kv : obj_)
        if (kv.first == key) return kv.second;
    return nullValue();
}

bool JsonValue::has(const char* key) const {
    if (type_ != Type::Object || !key) return false;
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

bool JsonValue::isNumericString() const {
    return type_ == Type::String && numericText(str_, nullptr);
}

long long JsonValue::asInt64(long long def) const {
    long long v = 0;
    switch (type_) {
        case Type::Number:
            // str_ holds the original token, so an exact integer stays exact.
            if (integerText(str_, &v)) return v;
            return static_cast<long long>(num_);
        case Type::String:
            if (integerText(str_, &v)) return v;
            {   // "7.8" as an int is 7 — a panel that quotes a float where an int belongs
                double d = 0;
                if (numericText(str_, &d)) return static_cast<long long>(d);
            }
            return def;
        case Type::Bool: return bool_ ? 1 : 0;
        default: return def;
    }
}

int JsonValue::asInt(int def) const {
    const long long v = asInt64(def);
    if (v > 0x7FFFFFFFll) return 0x7FFFFFFF;
    if (v < -0x7FFFFFFFll - 1) return -0x7FFFFFFF - 1;
    return static_cast<int>(v);
}

double JsonValue::asDouble(double def) const {
    double d = 0;
    switch (type_) {
        case Type::Number: return num_;
        case Type::String: return numericText(str_, &d) ? d : def;
        case Type::Bool:   return bool_ ? 1.0 : 0.0;
        default: return def;
    }
}

bool JsonValue::asBool(bool def) const {
    switch (type_) {
        case Type::Bool:   return bool_;
        // Panels express flags as 0/1, and as "0"/"1" — auth, is_trial and the like.
        case Type::Number: return num_ != 0.0;
        case Type::String: {
            if (str_ == "true" || str_ == "yes") return true;
            if (str_ == "false" || str_ == "no" || str_.empty()) return false;
            double d = 0;
            if (numericText(str_, &d)) return d != 0.0;
            return def;
        }
        default: return def;
    }
}

std::string JsonValue::asString(const std::string& def) const {
    switch (type_) {
        case Type::String: return str_;
        case Type::Number: return str_;  // the original token, not a reformatted double
        case Type::Bool:   return bool_ ? "true" : "false";
        default: return def;
    }
}

std::wstring JsonValue::asWString() const { return wideFromUtf8(asString()); }

// ---------------------------------------------------------------------------

class JsonParser {
public:
    JsonParser(const std::string& s, std::string* err) : s_(s), err_(err) {}

    bool run(JsonValue& out) {
        // A UTF-8 BOM is accepted and skipped: a panel whose PHP file was saved with one is
        // misconfigured, not broken, and rejecting it would fail a provider that works.
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB && static_cast<unsigned char>(s_[2]) == 0xBF)
            i_ = 3;
        skipWs();
        if (i_ >= s_.size()) return fail("empty document");
        if (!value(out, 0)) return false;
        skipWs();
        // Trailing content is an ERROR, not something to ignore. A PHP warning appended
        // after the JSON is the signature of a server fault, and a client that shrugs it off
        // would import a partial library over a good one.
        if (i_ < s_.size()) return fail("unexpected content after the root value");
        return true;
    }

private:
    const std::string& s_;
    std::string* err_;
    size_t i_ = 0;

    bool fail(const char* what) {
        if (err_) {
            std::string ctx = s_.substr(i_ > 24 ? i_ - 24 : 0, 48);
            for (char& c : ctx)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            *err_ = std::string(what) + " at byte " + std::to_string(i_) + " near: " + ctx;
        }
        return false;
    }

    void skipWs() {
        while (i_ < s_.size() && isWs(s_[i_])) ++i_;
    }

    static void appendUtf8(std::string& o, unsigned long cp) {
        if (cp <= 0x7F) { o += static_cast<char>(cp); }
        else if (cp <= 0x7FF) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned long& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + k];
            unsigned long d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned long>(c - 'A' + 10);
            else return false;
            out = (out << 4) | d;
        }
        i_ += 4;
        return true;
    }

    bool string(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return fail("expected a string");
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '"') { ++i_; return true; }
            if (c != '\\') { out += c; ++i_; continue; }
            if (++i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() && s_[i_] == '\\' &&
                        s_[i_ + 1] == 'u') {
                        const size_t save = i_;
                        i_ += 2;
                        unsigned long lo = 0;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            i_ = save;
                    }
                    // An unpaired surrogate has no UTF-8 encoding. Substituting U+FFFD keeps
                    // the string valid UTF-8 all the way into SQLite, which would otherwise
                    // reject the row and lose a whole film over one bad title byte.
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool value(JsonValue& v, int depth) {
        if (depth > kJsonMaxDepth) return fail("nesting too deep");
        skipWs();
        if (i_ >= s_.size()) return fail("unexpected end of document");
        const char c = s_[i_];

        if (c == '{') {
            v.type_ = JsonValue::Type::Object;
            ++i_;
            skipWs();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                skipWs();
                std::string key;
                if (!string(key)) return false;
                skipWs();
                if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
                ++i_;
                JsonValue child;
                if (!value(child, depth + 1)) return false;
                // Duplicate keys: last wins, which is what every mainstream parser does, so a
                // panel emitting one gets the same answer here as in its own test suite.
                bool replaced = false;
                for (auto& kv : v.obj_)
                    if (kv.first == key) { kv.second = std::move(child); replaced = true; break; }
                if (!replaced) v.obj_.emplace_back(std::move(key), std::move(child));
                skipWs();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                return fail("expected ',' or '}'");
            }
        }

        if (c == '[') {
            v.type_ = JsonValue::Type::Array;
            ++i_;
            skipWs();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            for (;;) {
                JsonValue child;
                if (!value(child, depth + 1)) return false;
                v.arr_.push_back(std::move(child));
                skipWs();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                return fail("expected ',' or ']'");
            }
        }

        if (c == '"') {
            v.type_ = JsonValue::Type::String;
            return string(v.str_);
        }
        if (s_.compare(i_, 4, "true") == 0)  { i_ += 4; v.type_ = JsonValue::Type::Bool; v.bool_ = true;  return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; v.type_ = JsonValue::Type::Bool; v.bool_ = false; return true; }
        if (s_.compare(i_, 4, "null") == 0)  { i_ += 4; v.type_ = JsonValue::Type::Null; return true; }

        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            const size_t start = i_;
            if (s_[i_] == '-' || s_[i_] == '+') ++i_;
            bool digits = false;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; digits = true; }
            if (i_ < s_.size() && s_[i_] == '.') {
                ++i_;
                while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; digits = true; }
            }
            if (!digits) return fail("a number with no digits");
            if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
                ++i_;
                if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
                bool expDigits = false;
                while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') { ++i_; expDigits = true; }
                if (!expDigits) return fail("an exponent with no digits");
            }
            v.type_ = JsonValue::Type::Number;
            v.str_ = s_.substr(start, i_ - start);  // keep the token: 64-bit ids stay exact
            v.num_ = std::strtod(v.str_.c_str(), nullptr);
            return true;
        }
        return fail("unexpected character");
    }
};

bool parseJson(const std::string& text, JsonValue& out, std::string* error) {
    out = JsonValue{};
    if (error) error->clear();
    JsonParser p(text, error);
    return p.run(out);
}

}  // namespace rabbitears
