#include "a70/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace a70 {

Json& Json::operator[](const std::string& key) {
    if (kind_ != Kind::Object) {
        *this = object();
    }
    for (auto& kv : obj_)
        if (kv.first == key) return kv.second;
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

const Json* Json::find(const std::string& key) const {
    for (auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void Json::push(Json v) {
    if (kind_ != Kind::Array) *this = array();
    arr_.push_back(std::move(v));
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

std::string Json::scalarString() const {
    switch (kind_) {
        case Kind::Null: return "";
        case Kind::Bool: return b_ ? "true" : "false";
        case Kind::Int: return std::to_string(i_);
        case Kind::Number: {
            char b[40];
            std::snprintf(b, sizeof b, "%.10g", d_);
            return b;
        }
        case Kind::String: return s_;
        default: return dump();
    }
}

void Json::dumpTo(std::string& out, int indent, int level) const {
    auto nl = [&](int lv) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * lv), ' ');
    };
    switch (kind_) {
        case Kind::Null: out += "null"; break;
        case Kind::Bool: out += b_ ? "true" : "false"; break;
        case Kind::Int: out += std::to_string(i_); break;
        case Kind::Number:
            if (std::isfinite(d_)) {
                char b[40];
                std::snprintf(b, sizeof b, "%.10g", d_);
                out += b;
            } else {
                out += "null";
            }
            break;
        case Kind::String: out += '"' + jsonEscape(s_) + '"'; break;
        case Kind::Array: {
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += indent < 0 ? ", " : ",";
                if (indent >= 0 && arr_[i].kind_ >= Kind::Array) nl(level + 1);
                arr_[i].dumpTo(out, indent, level + 1);
            }
            out += ']';
            break;
        }
        case Kind::Object: {
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                nl(level + 1);
                out += '"' + jsonEscape(obj_[i].first) + "\": ";
                obj_[i].second.dumpTo(out, indent, level + 1);
            }
            if (!obj_.empty()) nl(level);
            out += '}';
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string s;
    dumpTo(s, indent, 0);
    return s;
}

bool jsonFindNumber(const std::string& json, const std::string& key, double& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    const char* s = json.c_str() + p + 1;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s) return false;
    out = v;
    return true;
}

}  // namespace a70
