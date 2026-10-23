// Minimal JSON value, parser and writer for presets and settings.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(int v) : type(Type::Number), num(v) {}
    Value(double v) : type(Type::Number), num(v) {}
    Value(float v) : type(Type::Number), num(v) {}
    Value(const char* v) : type(Type::String), str(v) {}
    Value(std::string v) : type(Type::String), str(std::move(v)) {}

    static Value array() { Value v; v.type = Type::Array; return v; }
    static Value object() { Value v; v.type = Type::Object; return v; }

    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }

    const Value* find(const std::string& key) const {
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Value& operator[](const std::string& key) {
        if (type != Type::Object) { type = Type::Object; obj.clear(); }
        for (auto& kv : obj)
            if (kv.first == key) return kv.second;
        obj.emplace_back(key, Value());
        return obj.back().second;
    }
    void push(Value v) {
        if (type != Type::Array) { type = Type::Array; arr.clear(); }
        arr.push_back(std::move(v));
    }

    double getNum(const std::string& key, double def) const {
        auto* v = find(key);
        return (v && v->type == Type::Number) ? v->num : def;
    }
    bool getBool(const std::string& key, bool def) const {
        auto* v = find(key);
        return (v && v->type == Type::Bool) ? v->b : def;
    }
    std::string getStr(const std::string& key, const std::string& def = {}) const {
        auto* v = find(key);
        return (v && v->type == Type::String) ? v->str : def;
    }
};

namespace detail {
struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool lit(const char* s) {
        const char* q = p;
        while (*s) {
            if (q >= end || *q != *s) return false;
            ++q; ++s;
        }
        p = q;
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& cp) {
        if (end - p < 4) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    std::string string() {
        std::string out;
        ++p;  // opening quote
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { out += c; continue; }
            if (p >= end) { ok = false; break; }
            char e = *p++;
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) { ok = false; return out; }
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo;
                        if (!hex4(lo)) { ok = false; return out; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
        if (p >= end) ok = false; else ++p;
        return out;
    }
    Value value(int depth) {
        ws();
        if (p >= end || depth > 64) { ok = false; return {}; }
        char c = *p;
        if (c == '{') {
            ++p;
            Value v = Value::object();
            ws();
            if (p < end && *p == '}') { ++p; return v; }
            while (ok) {
                ws();
                if (p >= end || *p != '"') { ok = false; break; }
                std::string k = string();
                ws();
                if (p >= end || *p != ':') { ok = false; break; }
                ++p;
                v.obj.emplace_back(std::move(k), value(depth + 1));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; break; }
                ok = false;
            }
            return v;
        }
        if (c == '[') {
            ++p;
            Value v = Value::array();
            ws();
            if (p < end && *p == ']') { ++p; return v; }
            while (ok) {
                v.arr.push_back(value(depth + 1));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; break; }
                ok = false;
            }
            return v;
        }
        if (c == '"') return Value(string());
        if (lit("true")) return Value(true);
        if (lit("false")) return Value(false);
        if (lit("null")) return Value();
        char* q = nullptr;
        std::string tmp(p, size_t(std::min<ptrdiff_t>(end - p, 64)));
        double d = std::strtod(tmp.c_str(), &q);
        if (q == tmp.c_str()) { ok = false; return {}; }
        p += (q - tmp.c_str());
        return Value(d);
    }
};

inline void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += char(c);
        }
    }
    out += '"';
}

inline void write(std::string& out, const Value& v, int indent) {
    auto nl = [&](int n) { out += '\n'; out.append(size_t(n) * 2, ' '); };
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.b ? "true" : "false"; break;
        case Value::Type::Number: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.6g", v.num);
            out += buf;
            break;
        }
        case Value::Type::String: escape(out, v.str); break;
        case Value::Type::Array:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1);
                write(out, v.arr[i], indent + 1);
            }
            if (!v.arr.empty()) nl(indent);
            out += ']';
            break;
        case Value::Type::Object:
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1);
                escape(out, v.obj[i].first);
                out += ": ";
                write(out, v.obj[i].second, indent + 1);
            }
            if (!v.obj.empty()) nl(indent);
            out += '}';
            break;
    }
}
}  // namespace detail

inline bool parse(const std::string& text, Value& out) {
    detail::Parser ps{text.data(), text.data() + text.size()};
    out = ps.value(0);
    ps.ws();
    return ps.ok;
}

inline std::string dump(const Value& v) {
    std::string out;
    detail::write(out, v, 0);
    out += '\n';
    return out;
}

}  // namespace json
