// SPDX-License-Identifier: GPL-3.0-or-later
//
// Json — a small, dependency-free JSON reader for the Xtream `player_api.php` client.
//
// WHY HAND-ROLLED: house style. The M3U and XMLTV parsers are hand-rolled too; vendoring is
// reserved for things that are genuinely hard (SQLite, miniz). This is ~400 lines and buys a
// dependency-free shared core on both platforms.
//
// WHY IT IS SHAPED THE WAY IT IS — every decision below traces to a real property of Xtream
// panels, established by the `RabbitEarsCli --xtream` reconnaissance probe:
//
//   • NUMBERS ARRIVE AS QUOTED STRINGS, inconsistently, even within one response and one
//     field across items. `{"stream_id":1234}` and `{"stream_id":"1234"}` are both common,
//     and a panel will mix them. So the SCALAR ACCESSORS ARE TOLERANT BY DESIGN: asInt64()
//     reads a JSON number or a numeric string, and asString() reads a string or renders a
//     number. Typed access is where a strict parser would hand you a crash on a provider
//     you never tested against, so the tolerance lives here rather than at every call site.
//     It is deliberately NOT a "lenient mode" flag — there is no configuration in which the
//     caller wants to hard-fail on a quoted number.
//   • FIELDS ARE OPTIONAL AND VARY BY PANEL. Missing is normal, not exceptional, so
//     operator[] on an absent key returns a shared null value rather than throwing or
//     inserting — chained reads like `root["info"]["name"].asString()` are always safe.
//   • OBJECTS ARE SMALL (tens of keys) AND ORDER MATTERS FOR DIAGNOSTICS, so members are a
//     vector with linear lookup, not a map. Lower overhead at this size, and it keeps the
//     document order the recon report shows.
//
// STRUCTURE, by contrast, IS PARSED STRICTLY: a malformed document is an error with a byte
// offset, not a best guess. Tolerating bad structure would hide a truncated download, which
// is the failure most likely to silently destroy a user's library on a sync.
//
// NOT a general-purpose JSON library: no serialization, no mutation, no comments/NaN
// extensions. It reads what a panel sends and nothing more.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rabbitears {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Elements for an array, members for an object, 0 otherwise.
    size_t size() const;

    // ---- containers ---------------------------------------------------------
    // Both return a shared NULL value when the index/key is absent or this is not the right
    // container type, so a chain over a panel that omits half the fields cannot fault.
    const JsonValue& operator[](size_t i) const;
    // `int` overload on purpose: without it `v[0]` is AMBIGUOUS, because the literal 0
    // converts equally well to size_t and to a null const char*. An exact int match settles
    // it, and keeps `v["key"]` on the pointer overload with no std::string temporary — which
    // matters when a 40k-item import reads a dozen fields per item.
    const JsonValue& operator[](int i) const { return (*this)[static_cast<size_t>(i < 0 ? 0 : i)]; }
    const JsonValue& operator[](const char* key) const;
    const JsonValue& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
    bool has(const char* key) const;

    // Object members in document order. Used by the series browser, where `episodes` is a MAP
    // KEYED BY SEASON NUMBER rather than an array, so the keys carry data.
    const std::vector<std::pair<std::string, JsonValue>>& members() const { return obj_; }
    const std::vector<JsonValue>& elements() const { return arr_; }

    // ---- tolerant scalars ---------------------------------------------------
    // These accept the value in EITHER of the spellings a panel uses. See the header note:
    // this is the single most important property of this parser.
    long long   asInt64(long long def = 0) const;
    int         asInt(int def = 0) const;
    double      asDouble(double def = 0.0) const;
    bool        asBool(bool def = false) const;
    std::string asString(const std::string& def = {}) const;  // UTF-8
    std::wstring asWString() const;

    // True when this value is a string whose CONTENT is a number — the shape that breaks
    // strict clients. Exposed so an importer can log which fields a panel quotes.
    bool isNumericString() const;

private:
    friend class JsonParser;
    Type        type_ = Type::Null;
    bool        bool_ = false;
    double      num_ = 0.0;
    std::string str_;  // string value, or the number's ORIGINAL text (so a 64-bit id that
                       // does not survive a double round-trip is still readable exactly)
    std::vector<JsonValue> arr_;
    std::vector<std::pair<std::string, JsonValue>> obj_;
};

// Parse UTF-8 `text` into `out`. Returns false on a structural error and, when `error` is
// non-null, fills it with a human-readable message INCLUDING A BYTE OFFSET — a truncated or
// error-page response must be diagnosable from a user's log without the body.
//
// Accepts a leading UTF-8 BOM (a real, common panel misconfiguration). Rejects trailing
// content after the root value: a PHP notice appended to an otherwise valid body would
// otherwise parse "successfully" and silently truncate nothing while hiding a server fault.
bool parseJson(const std::string& text, JsonValue& out, std::string* error = nullptr);

// Maximum nesting accepted before parsing fails. Guards the recursive descent against a
// hostile or corrupt body; far above anything player_api.php produces (its deepest real
// shape, series_info.episodes.<season>[].info, is 5).
constexpr int kJsonMaxDepth = 64;

}  // namespace rabbitears
