#pragma once
// Minimal ordered JSON value (writer only) for metadata sidecars.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace a70 {

class Json {
public:
    enum class Kind { Null, Bool, Number, Int, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : kind_(Kind::Bool), b_(b) {}
    Json(int v) : kind_(Kind::Int), i_(v) {}
    Json(long v) : kind_(Kind::Int), i_(v) {}
    Json(long long v) : kind_(Kind::Int), i_(v) {}
    Json(unsigned v) : kind_(Kind::Int), i_(v) {}
    Json(unsigned long v) : kind_(Kind::Int), i_(static_cast<int64_t>(v)) {}
    Json(unsigned long long v) : kind_(Kind::Int), i_(static_cast<int64_t>(v)) {}
    Json(double v) : kind_(Kind::Number), d_(v) {}
    Json(float v) : kind_(Kind::Number), d_(v) {}
    Json(const char* s) : kind_(Kind::String), s_(s) {}
    Json(std::string s) : kind_(Kind::String), s_(std::move(s)) {}

    static Json array() { Json j; j.kind_ = Kind::Array; return j; }
    static Json object() { Json j; j.kind_ = Kind::Object; return j; }

    Kind kind() const { return kind_; }
    bool isObject() const { return kind_ == Kind::Object; }

    // object access (insertion ordered)
    Json& operator[](const std::string& key);
    const Json* find(const std::string& key) const;
    void push(Json v);  // array append
    const std::vector<std::pair<std::string, Json>>& items() const { return obj_; }
    const std::vector<Json>& elements() const { return arr_; }

    double number() const { return kind_ == Kind::Int ? static_cast<double>(i_) : d_; }
    const std::string& str() const { return s_; }
    std::string scalarString() const;  // value as plain text (no quotes for strings)

    std::string dump(int indent = -1) const;

private:
    void dumpTo(std::string& out, int indent, int level) const;
    Kind kind_ = Kind::Null;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

std::string jsonEscape(const std::string& s);
// Tiny extractor for "key": number from our own JSON (used when reading sidecars).
bool jsonFindNumber(const std::string& json, const std::string& key, double& out);

}  // namespace a70
