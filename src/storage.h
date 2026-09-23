// WorkBoard persistence: save / restore workspaces (names, notes, view state)
// to a JSON file. Header-only, dependency-free (C++20).
//
// Design notes:
//  - Every UI mutation notifies C++ through the `persist` callback, so the
//    file is rewritten immediately without any extra user input.
//  - A final save also runs after the event loop exits (window closed).
//  - Loading is defensive: a missing or corrupt file falls back to a single
//    empty workspace instead of failing.
//  - Saving is best-effort and never throws: persistence must never crash
//    the app. Writes go to a temp file + rename to avoid corrupt files.

#pragma once

#include "app.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace workboard {
namespace detail {

struct ParseError {};

// Minimal JSON value model (only what we need for load).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;

    static JsonValue make_bool(bool b) {
        JsonValue v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    }

    // Missing keys / type mismatches yield a null sentinel, so callers can
    // chain lookups with defaults instead of null checks.
    const JsonValue &field(std::string_view key) const {
        static const JsonValue kNull;
        if (type != Type::Object)
            return kNull;
        for (const auto &f : fields) {
            if (f.first == key)
                return f.second;
        }
        return kNull;
    }

    std::string_view as_string(std::string_view dflt = {}) const {
        return type == Type::String ? std::string_view(str) : dflt;
    }

    double as_number(double dflt = 0.0) const {
        return type == Type::Number ? number : dflt;
    }

    long as_int(long dflt = 0) const {
        if (type != Type::Number)
            return dflt;
        if (number > 1000000000.0 || number < -1000000000.0)
            return dflt; // out of sane range (also guards std::lround)
        return static_cast<long>(std::lround(number));
    }

    bool as_bool(bool dflt = false) const {
        return type == Type::Bool ? boolean : dflt;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s(text) {}

    JsonValue parse() {
        skip_ws();
        JsonValue v = parse_value();
        skip_ws();
        if (pos != s.size())
            fail();
        return v;
    }

private:
    std::string_view s;
    std::size_t pos = 0;

    [[noreturn]] void fail() { throw ParseError{}; }

    char peek() const { return pos < s.size() ? s[pos] : '\0'; }
    char take() { return pos < s.size() ? s[pos++] : '\0'; }

    void expect(char c) {
        if (take() != c)
            fail();
    }

    void skip_ws() {
        while (pos < s.size()) {
            char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos;
            else
                break;
        }
    }

    void expect_literal(std::string_view lit) {
        if (s.substr(pos, lit.size()) != lit)
            fail();
        pos += lit.size();
    }

    JsonValue parse_value() {
        char c = peek();
        if (c == '{')
            return parse_object();
        if (c == '[')
            return parse_array();
        if (c == '"') {
            JsonValue v;
            v.type = JsonValue::Type::String;
            v.str = parse_string();
            return v;
        }
        if (c == 't') {
            expect_literal("true");
            return JsonValue::make_bool(true);
        }
        if (c == 'f') {
            expect_literal("false");
            return JsonValue::make_bool(false);
        }
        if (c == 'n') {
            expect_literal("null");
            return JsonValue{};
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            JsonValue v;
            v.type = JsonValue::Type::Number;
            v.number = parse_number();
            return v;
        }
        fail();
    }

    JsonValue parse_array() {
        expect('[');
        JsonValue v;
        v.type = JsonValue::Type::Array;
        skip_ws();
        if (peek() == ']') {
            take();
            return v;
        }
        while (true) {
            skip_ws();
            v.items.push_back(parse_value());
            skip_ws();
            char c = take();
            if (c == ']')
                break;
            if (c != ',')
                fail();
        }
        return v;
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue v;
        v.type = JsonValue::Type::Object;
        skip_ws();
        if (peek() == '}') {
            take();
            return v;
        }
        while (true) {
            skip_ws();
            if (peek() != '"')
                fail();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            v.fields.emplace_back(std::move(key), parse_value());
            skip_ws();
            char c = take();
            if (c == '}')
                break;
            if (c != ',')
                fail();
        }
        return v;
    }

    // Locale-independent number parsing (never touches the C locale).
    double parse_number() {
        bool neg = false;
        if (peek() == '-') {
            neg = true;
            take();
        }
        if (peek() < '0' || peek() > '9')
            fail();
        double v = 0.0;
        while (peek() >= '0' && peek() <= '9')
            v = v * 10.0 + (take() - '0');
        if (peek() == '.') {
            take();
            if (peek() < '0' || peek() > '9')
                fail();
            double frac = 0.0;
            double base = 1.0;
            while (peek() >= '0' && peek() <= '9') {
                frac = frac * 10.0 + (take() - '0');
                base *= 10.0;
            }
            v += frac / base;
        }
        if (peek() == 'e' || peek() == 'E') {
            take();
            bool exp_neg = false;
            if (peek() == '+' || peek() == '-')
                exp_neg = (take() == '-');
            if (peek() < '0' || peek() > '9')
                fail();
            int e = 0;
            while (peek() >= '0' && peek() <= '9')
                e = e * 10 + (take() - '0');
            v *= std::pow(10.0, exp_neg ? -e : e);
        }
        return neg ? -v : v;
    }

    unsigned parse_hex4() {
        if (pos + 4 > s.size())
            fail();
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[pos++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail();
        }
        return v;
    }

    static void encode_utf8(std::string &out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos >= s.size())
                fail();
            char c = take();
            if (c == '"')
                break;
            if (c == '\\') {
                char e = take();
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (take() != '\\' || take() != 'u')
                            fail();
                        unsigned lo = parse_hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            fail();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail(); // lone low surrogate
                    }
                    encode_utf8(out, cp);
                    break;
                }
                default: fail();
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                fail(); // unescaped control character
            } else {
                out += c; // raw byte (UTF-8 passes through untouched)
            }
        }
        return out;
    }
};

inline void write_json_string(std::string &out, std::string_view str) {
    out += '"';
    for (unsigned char c : str) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

// Locale-independent float formatting (shortest round-trip).
inline void write_json_number(std::string &out, double v) {
    if (!std::isfinite(v)) {
        out += '0';
        return;
    }
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof buf, v);
    if (res.ec == std::errc())
        out.append(buf, res.ptr);
    else
        out += '0';
}

inline std::string color_to_string(const slint::Color &c) {
    char buf[10];
    if (c.alpha() == 255)
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.red(), c.green(), c.blue());
    else
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", c.red(), c.green(), c.blue(),
                      c.alpha());
    return std::string(buf);
}

inline slint::Color color_from_string(std::string_view text) {
    const uint8_t d_r = 0xFF, d_g = 0xF3, d_b = 0xCD; // default sticky-note yellow
    std::string_view h = text;
    if (!h.empty() && h.front() == '#')
        h.remove_prefix(1);

    auto hex_val = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    auto nibble = [&](std::size_t i) -> int {
        return i < h.size() ? hex_val(h[i]) : -1;
    };

    if (h.size() == 3 || h.size() == 4) {
        int r = nibble(0), g = nibble(1), b = nibble(2);
        if (r < 0 || g < 0 || b < 0)
            return slint::Color::from_rgb_uint8(d_r, d_g, d_b);
        uint8_t a = 0xFF;
        if (h.size() == 4) {
            int av = nibble(3);
            if (av < 0)
                return slint::Color::from_rgb_uint8(d_r, d_g, d_b);
            a = static_cast<uint8_t>((av << 4) | av);
        }
        return slint::Color::from_argb_uint8(
            a, static_cast<uint8_t>((r << 4) | r), static_cast<uint8_t>((g << 4) | g),
            static_cast<uint8_t>((b << 4) | b));
    }
    if (h.size() == 6 || h.size() == 8) {
        auto byte_at = [&](std::size_t i) -> int {
            int hi = nibble(i * 2), lo = nibble(i * 2 + 1);
            return (hi < 0 || lo < 0) ? -1 : ((hi << 4) | lo);
        };
        int r = byte_at(0), g = byte_at(1), b = byte_at(2);
        if (r < 0 || g < 0 || b < 0)
            return slint::Color::from_rgb_uint8(d_r, d_g, d_b);
        uint8_t a = 0xFF;
        if (h.size() == 8) {
            int av = byte_at(3);
            if (av < 0)
                return slint::Color::from_rgb_uint8(d_r, d_g, d_b);
            a = static_cast<uint8_t>(av);
        }
        return slint::Color::from_argb_uint8(a, static_cast<uint8_t>(r),
                                             static_cast<uint8_t>(g), static_cast<uint8_t>(b));
    }
    return slint::Color::from_rgb_uint8(d_r, d_g, d_b);
}

} // namespace detail

// Platform data directory: ~/.local/share/WorkBoard (Linux),
// ~/Library/Application Support/WorkBoard (macOS), %APPDATA%/WorkBoard (Windows).
inline std::filesystem::path data_file() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    if (const char *a = std::getenv("APPDATA"))
        return fs::path(a) / "WorkBoard" / "workspaces.json";
    if (const char *u = std::getenv("USERPROFILE"))
        return fs::path(u) / "AppData" / "Roaming" / "WorkBoard" / "workspaces.json";
    return fs::path("WorkBoard") / "workspaces.json";
#elif defined(__APPLE__)
    if (const char *h = std::getenv("HOME"))
        return fs::path(h) / "Library" / "Application Support" / "WorkBoard" / "workspaces.json";
    return fs::path("WorkBoard") / "workspaces.json";
#else
    if (const char *x = std::getenv("XDG_DATA_HOME")) {
        if (*x != '\0')
            return fs::path(x) / "WorkBoard" / "workspaces.json";
    }
    if (const char *h = std::getenv("HOME"))
        return fs::path(h) / ".local" / "share" / "WorkBoard" / "workspaces.json";
    return fs::path("WorkBoard") / "workspaces.json";
#endif
}

// Read the save file and push the state into the UI. `ws-active` is set last
// so the UI loads the active workspace's view once everything else is ready.
inline void load_state(App &app) {
    std::vector<std::string> d_names = { "Workspace 1" };
    std::vector<float> d_zooms = { 1.0f };
    std::vector<float> d_oxs = { 0.0f };
    std::vector<float> d_oys = { 0.0f };
    std::vector<StickyNote> d_notes;
    int d_active = 0;
    bool d_sidebar = true;

    try {
        std::ifstream in(data_file(), std::ios::binary);
        if (!in)
            throw detail::ParseError{};
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        // Tolerate a UTF-8 BOM (some Windows editors add one).
        if (text.size() >= 3 && text[0] == '\xEF' && text[1] == '\xBB' && text[2] == '\xBF')
            text.erase(0, 3);
        const detail::JsonValue root = detail::JsonParser(text).parse();

        const detail::JsonValue &ws_arr = root.field("workspaces");
        if (ws_arr.type != detail::JsonValue::Type::Array || ws_arr.items.empty())
            throw detail::ParseError{};

        d_names.clear();
        d_zooms.clear();
        d_oxs.clear();
        d_oys.clear();
        d_notes.clear();

        int ws_index = 0;
        for (const auto &w : ws_arr.items) {
            if (w.type != detail::JsonValue::Type::Object)
                continue; // skip corrupt entries, keep the rest
            d_names.push_back(std::string(w.field("name").as_string("Workspace")));
            float zoom = static_cast<float>(w.field("zoom").as_number(1.0));
            d_zooms.push_back(zoom > 0.0f ? zoom : 1.0f);
            d_oxs.push_back(static_cast<float>(w.field("ox").as_number(0.0)));
            d_oys.push_back(static_cast<float>(w.field("oy").as_number(0.0)));

            const detail::JsonValue &ns = w.field("notes");
            if (ns.type == detail::JsonValue::Type::Array) {
                for (const auto &n : ns.items) {
                    if (n.type != detail::JsonValue::Type::Object)
                        continue;
                    StickyNote sn;
                    sn.wx = static_cast<float>(n.field("x").as_number(0.0));
                    sn.wy = static_cast<float>(n.field("y").as_number(0.0));
                    sn.color = detail::color_from_string(n.field("color").as_string());
                    sn.title = slint::SharedString(n.field("title").as_string());
                    sn.text = slint::SharedString(n.field("text").as_string());
                    sn.ws = static_cast<int>(n.field("ws").as_int(ws_index));
                    d_notes.push_back(sn);
                }
            }
            ++ws_index;
        }
        if (d_names.empty())
            throw detail::ParseError{};

        // Clamp note workspace tags (they may reference workspaces parsed later).
        for (auto &n : d_notes) {
            if (n.ws < 0 || n.ws >= static_cast<int>(d_names.size()))
                n.ws = 0;
        }

        long active = root.field("active").as_int(0);
        d_active = (active >= 0 && active < static_cast<long>(d_names.size()))
            ? static_cast<int>(active)
            : 0;
        d_sidebar = root.field("sidebar").as_bool(true);
    } catch (...) {
        // Keep the defaults above: a missing/corrupt file starts fresh.
        d_names = { "Workspace 1" };
        d_zooms = { 1.0f };
        d_oxs = { 0.0f };
        d_oys = { 0.0f };
        d_notes.clear();
        d_active = 0;
        d_sidebar = true;
    }

    // Per-workspace note counts (used for the empty-canvas hint).
    std::vector<int> d_counts(d_names.size(), 0);
    for (const auto &n : d_notes) {
        if (n.ws >= 0 && n.ws < static_cast<int>(d_counts.size()))
            ++d_counts[n.ws];
    }

    auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const auto &n : d_names)
        names->push_back(slint::SharedString(std::string_view(n)));
    auto zooms = std::make_shared<slint::VectorModel<float>>();
    for (float v : d_zooms)
        zooms->push_back(v);
    auto oxs = std::make_shared<slint::VectorModel<float>>();
    for (float v : d_oxs)
        oxs->push_back(v);
    auto oys = std::make_shared<slint::VectorModel<float>>();
    for (float v : d_oys)
        oys->push_back(v);
    auto counts = std::make_shared<slint::VectorModel<int>>();
    for (int v : d_counts)
        counts->push_back(v);
    auto notes = std::make_shared<slint::VectorModel<StickyNote>>();
    for (const auto &n : d_notes)
        notes->push_back(n);

    app.set_ws_names(names);
    app.set_ws_zooms(zooms);
    app.set_ws_oxs(oxs);
    app.set_ws_oys(oys);
    app.set_notes(notes);
    app.set_ws_counts(counts);
    app.set_sidebar_open(d_sidebar);
    app.set_ws_active(d_active); // last: triggers the UI to show this workspace
}

// Serialize the full UI state and write it atomically (temp file + rename).
inline void save_state(const App &app) {
    try {
        const auto names = app.get_ws_names();
        const auto zooms = app.get_ws_zooms();
        const auto oxs = app.get_ws_oxs();
        const auto oys = app.get_ws_oys();
        const auto notes = app.get_notes();
        const std::size_t name_count = names ? names->row_count() : 0;
        const std::size_t note_count = notes ? notes->row_count() : 0;

        std::string out;
        out += "{\"version\":1,\"sidebar\":";
        out += app.get_sidebar_open() ? "true" : "false";
        out += ",\"active\":";
        out += std::to_string(app.get_ws_active());
        out += ",\"workspaces\":[";
        for (std::size_t i = 0; i < name_count; ++i) {
            if (i > 0)
                out += ',';
            std::optional<slint::SharedString> name_opt;
            if (names)
                name_opt = names->row_data(i);
            float zoom = 1.0f, ox = 0.0f, oy = 0.0f;
            if (zooms) {
                if (auto v = zooms->row_data(i))
                    zoom = *v;
            }
            if (oxs) {
                if (auto v = oxs->row_data(i))
                    ox = *v;
            }
            if (oys) {
                if (auto v = oys->row_data(i))
                    oy = *v;
            }
            out += "{\"name\":";
            detail::write_json_string(out, name_opt ? std::string_view(*name_opt)
                                                   : std::string_view{});
            out += ",\"zoom\":";
            detail::write_json_number(out, zoom);
            out += ",\"ox\":";
            detail::write_json_number(out, ox);
            out += ",\"oy\":";
            detail::write_json_number(out, oy);
            out += ",\"notes\":[";
            bool first = true;
            for (std::size_t j = 0; j < note_count; ++j) {
                std::optional<StickyNote> n_opt;
                if (notes)
                    n_opt = notes->row_data(j);
                if (!n_opt)
                    continue;
                const StickyNote &n = *n_opt;
                if (n.ws != static_cast<int>(i))
                    continue;
                if (!first)
                    out += ',';
                first = false;
                out += "{\"x\":";
                detail::write_json_number(out, n.wx);
                out += ",\"y\":";
                detail::write_json_number(out, n.wy);
                out += ",\"color\":";
                detail::write_json_string(out, detail::color_to_string(n.color));
                out += ",\"title\":";
                detail::write_json_string(out, std::string_view(n.title));
                out += ",\"text\":";
                detail::write_json_string(out, std::string_view(n.text));
                out += ",\"ws\":";
                out += std::to_string(n.ws);
                out += '}';
            }
            out += "]}";
        }
        out += "]}";

        std::error_code ec;
        std::filesystem::create_directories(data_file().parent_path(), ec);
        if (ec)
            return;
        std::filesystem::path tmp = data_file();
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f)
                return;
            f.write(out.data(), static_cast<std::streamsize>(out.size()));
            f.flush();
            if (!f)
                return;
        }
        std::filesystem::rename(tmp, data_file(), ec);
    } catch (...) {
        // best-effort: persistence must never crash the app
    }
}

} // namespace workboard
