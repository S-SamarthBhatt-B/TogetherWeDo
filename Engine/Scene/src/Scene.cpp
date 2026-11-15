#include "TogetherEngine/Scene/Scene.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <variant>

namespace together::scene {
namespace {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& result, std::string& error) {
        skip_space();
        if (!parse_value(result, error, 0)) return false;
        skip_space();
        if (cursor_ != input_.size()) return fail(error, "Unexpected characters after the JSON document.");
        return true;
    }

private:
    std::string_view input_;
    std::size_t cursor_{};

    void skip_space() {
        while (cursor_ < input_.size() && (input_[cursor_] == ' ' || input_[cursor_] == '\t' ||
            input_[cursor_] == '\r' || input_[cursor_] == '\n')) ++cursor_;
    }

    bool fail(std::string& error, std::string message) const {
        error = std::move(message) + " At byte " + std::to_string(cursor_) + ".";
        return false;
    }

    bool parse_value(JsonValue& output, std::string& error, const unsigned depth) {
        if (depth > 64) return fail(error, "JSON nesting exceeds the supported limit.");
        skip_space();
        if (cursor_ >= input_.size()) return fail(error, "Unexpected end of JSON input.");
        const char ch = input_[cursor_];
        if (ch == '{') return parse_object(output, error, depth + 1);
        if (ch == '[') return parse_array(output, error, depth + 1);
        if (ch == '"') {
            std::string text;
            if (!parse_string(text, error)) return false;
            output.value = std::move(text);
            return true;
        }
        if (ch == 't' && consume_literal("true")) { output.value = true; return true; }
        if (ch == 'f' && consume_literal("false")) { output.value = false; return true; }
        if (ch == 'n' && consume_literal("null")) { output.value = nullptr; return true; }
        if (ch == '-' || (ch >= '0' && ch <= '9')) return parse_number(output, error);
        return fail(error, "Invalid JSON value.");
    }

    bool consume_literal(const std::string_view literal) {
        if (input_.substr(cursor_, literal.size()) != literal) return false;
        cursor_ += literal.size();
        return true;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7F) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool parse_hex4(std::uint32_t& value, std::string& error) {
        if (cursor_ + 4 > input_.size()) return fail(error, "Incomplete Unicode escape.");
        value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char ch = input_[cursor_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value |= static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') value |= static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') value |= static_cast<unsigned>(ch - 'A' + 10);
            else return fail(error, "Invalid hexadecimal digit in Unicode escape.");
        }
        return true;
    }

    bool parse_string(std::string& output, std::string& error) {
        ++cursor_; // opening quote
        while (cursor_ < input_.size()) {
            const unsigned char ch = static_cast<unsigned char>(input_[cursor_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return fail(error, "Unescaped control character in JSON string.");
            if (ch != '\\') { output.push_back(static_cast<char>(ch)); continue; }
            if (cursor_ >= input_.size()) return fail(error, "Incomplete JSON string escape.");
            const char escaped = input_[cursor_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t first{};
                if (!parse_hex4(first, error)) return false;
                if (first >= 0xD800 && first <= 0xDBFF) {
                    if (cursor_ + 2 > input_.size() || input_[cursor_] != '\\' || input_[cursor_ + 1] != 'u')
                        return fail(error, "High surrogate is missing its low surrogate.");
                    cursor_ += 2;
                    std::uint32_t second{};
                    if (!parse_hex4(second, error)) return false;
                    if (second < 0xDC00 || second > 0xDFFF) return fail(error, "Invalid low surrogate.");
                    first = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                } else if (first >= 0xDC00 && first <= 0xDFFF) {
                    return fail(error, "Unexpected low surrogate.");
                }
                append_utf8(output, first);
                break;
            }
            default: return fail(error, "Unknown JSON string escape.");
            }
        }
        return fail(error, "Unterminated JSON string.");
    }

    bool parse_number(JsonValue& output, std::string& error) {
        const std::size_t start = cursor_;
        if (input_[cursor_] == '-') ++cursor_;
        if (cursor_ >= input_.size()) return fail(error, "Incomplete JSON number.");
        if (input_[cursor_] == '0') ++cursor_;
        else {
            if (input_[cursor_] < '1' || input_[cursor_] > '9') return fail(error, "Invalid JSON number.");
            while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9') ++cursor_;
        }
        if (cursor_ < input_.size() && input_[cursor_] == '.') {
            ++cursor_;
            const std::size_t fraction_start = cursor_;
            while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9') ++cursor_;
            if (cursor_ == fraction_start) return fail(error, "JSON fraction requires at least one digit.");
        }
        if (cursor_ < input_.size() && (input_[cursor_] == 'e' || input_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < input_.size() && (input_[cursor_] == '+' || input_[cursor_] == '-')) ++cursor_;
            const std::size_t exponent_start = cursor_;
            while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9') ++cursor_;
            if (cursor_ == exponent_start) return fail(error, "JSON exponent requires at least one digit.");
        }
        double number{};
        const char* first = input_.data() + start;
        const char* last = input_.data() + cursor_;
        const auto conversion = std::from_chars(first, last, number);
        if (conversion.ec != std::errc{} || conversion.ptr != last || !std::isfinite(number))
            return fail(error, "JSON number is outside the supported finite range.");
        output.value = number;
        return true;
    }

    bool parse_array(JsonValue& output, std::string& error, const unsigned depth) {
        ++cursor_;
        JsonValue::Array values;
        skip_space();
        if (cursor_ < input_.size() && input_[cursor_] == ']') { ++cursor_; output.value = std::move(values); return true; }
        while (cursor_ < input_.size()) {
            JsonValue value;
            if (!parse_value(value, error, depth)) return false;
            values.push_back(std::move(value));
            skip_space();
            if (cursor_ < input_.size() && input_[cursor_] == ']') { ++cursor_; output.value = std::move(values); return true; }
            if (cursor_ >= input_.size() || input_[cursor_++] != ',') return fail(error, "Expected ',' or ']' in JSON array.");
        }
        return fail(error, "Unterminated JSON array.");
    }

    bool parse_object(JsonValue& output, std::string& error, const unsigned depth) {
        ++cursor_;
        JsonValue::Object values;
        skip_space();
        if (cursor_ < input_.size() && input_[cursor_] == '}') { ++cursor_; output.value = std::move(values); return true; }
        while (cursor_ < input_.size()) {
            skip_space();
            if (cursor_ >= input_.size() || input_[cursor_] != '"') return fail(error, "JSON object keys must be strings.");
            std::string key;
            if (!parse_string(key, error)) return false;
            skip_space();
            if (cursor_ >= input_.size() || input_[cursor_++] != ':') return fail(error, "Expected ':' after JSON object key.");
            JsonValue value;
            if (!parse_value(value, error, depth)) return false;
            if (!values.emplace(std::move(key), std::move(value)).second) return fail(error, "Duplicate JSON object key.");
            skip_space();
            if (cursor_ < input_.size() && input_[cursor_] == '}') { ++cursor_; output.value = std::move(values); return true; }
            if (cursor_ >= input_.size() || input_[cursor_++] != ',') return fail(error, "Expected ',' or '}' in JSON object.");
        }
        return fail(error, "Unterminated JSON object.");
    }
};

const JsonValue* property(const JsonValue::Object& object, const std::string_view name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

bool read_number(const JsonValue::Object& object, const std::string_view key, double& output, std::string& error) {
    const JsonValue* value = property(object, key);
    if (!value || !std::holds_alternative<double>(value->value)) { error = "Expected numeric property '" + std::string(key) + "'."; return false; }
    output = std::get<double>(value->value);
    return true;
}

bool read_string(const JsonValue::Object& object, const std::string_view key, std::string& output, std::string& error) {
    const JsonValue* value = property(object, key);
    if (!value || !std::holds_alternative<std::string>(value->value)) { error = "Expected string property '" + std::string(key) + "'."; return false; }
    output = std::get<std::string>(value->value);
    return true;
}

bool read_bool(const JsonValue::Object& object, const std::string_view key, bool& output, std::string& error) {
    const JsonValue* value = property(object, key);
    if (!value || !std::holds_alternative<bool>(value->value)) { error = "Expected boolean property '" + std::string(key) + "'."; return false; }
    output = std::get<bool>(value->value);
    return true;
}

bool read_vec3(const JsonValue::Object& object, const std::string_view key, Vec3& output, std::string& error) {
    const JsonValue* value = property(object, key);
    if (!value || !std::holds_alternative<JsonValue::Array>(value->value)) { error = "Expected 3-number array property '" + std::string(key) + "'."; return false; }
    const auto& array = std::get<JsonValue::Array>(value->value);
    if (array.size() != 3 || !std::holds_alternative<double>(array[0].value) ||
        !std::holds_alternative<double>(array[1].value) || !std::holds_alternative<double>(array[2].value)) {
        error = "Property '" + std::string(key) + "' must contain exactly three numbers.";
        return false;
    }
    const double x = std::get<double>(array[0].value);
    const double y = std::get<double>(array[1].value);
    const double z = std::get<double>(array[2].value);
    if (std::abs(x) > 1.0e6 || std::abs(y) > 1.0e6 || std::abs(z) > 1.0e6) { error = "Vector value exceeds the supported range."; return false; }
    output = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    return true;
}

void write_string(std::ostream& output, const std::string& value) {
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20) output << "\\u00" << hex[ch >> 4] << hex[ch & 0x0F];
            else output << static_cast<char>(ch);
        }
    }
    output << '"';
}

void write_vec3(std::ostream& output, const Vec3& value) {
    output << '[' << value.x << ", " << value.y << ", " << value.z << ']';
}

bool object_from_json(const JsonValue& value, SceneObject& result, std::string& error) {
    if (!std::holds_alternative<JsonValue::Object>(value.value)) { error = "Each scene object must be a JSON object."; return false; }
    const auto& object = std::get<JsonValue::Object>(value.value);
    double id{};
    std::string name;
    if (!read_number(object, "id", id, error) || !read_string(object, "name", name, error)) return false;
    if (id < 1 || id > 9007199254740991.0 || std::floor(id) != id) {
        error = "Scene object id must be a positive integer.";
        return false;
    }
    result.id = static_cast<std::uint64_t>(id);
    result.name = std::move(name);
    const JsonValue* transform_value = property(object, "transform");
    if (!transform_value || !std::holds_alternative<JsonValue::Object>(transform_value->value)) { error = "Scene object requires a transform object."; return false; }
    const auto& transform = std::get<JsonValue::Object>(transform_value->value);
    if (!read_vec3(transform, "position", result.transform.position, error) ||
        !read_vec3(transform, "rotation", result.transform.rotation, error) ||
        !read_vec3(transform, "scale", result.transform.scale, error)) return false;
    const JsonValue* material_value = property(object, "material");
    if (!material_value || !std::holds_alternative<JsonValue::Object>(material_value->value)) { error = "Scene object requires a material object."; return false; }
    const auto& material = std::get<JsonValue::Object>(material_value->value);
    double roughness{}, metallic{};
    if (!read_vec3(material, "baseColor", result.material.base_color, error) ||
        !read_number(material, "roughness", roughness, error) ||
        !read_number(material, "metallic", metallic, error) ||
        !read_bool(object, "visible", result.visible, error)) return false;
    result.material.roughness = static_cast<float>(roughness);
    result.material.metallic = static_cast<float>(metallic);
    const JsonValue* emission = property(material, "emission");
    if (emission && !read_vec3(material, "emission", result.material.emission, error)) return false;
    const JsonValue* material_texture = property(material, "baseColorTextureAssetId");
    if (material_texture && !read_string(material, "baseColorTextureAssetId", result.material.base_color_texture_id, error)) return false;
    const JsonValue* normal_texture = property(material, "normalTextureAssetId");
    if (normal_texture && !read_string(material, "normalTextureAssetId", result.material.normal_texture_id, error)) return false;
    const JsonValue* metallic_roughness_texture = property(material, "metallicRoughnessTextureAssetId");
    if (metallic_roughness_texture && !read_string(material, "metallicRoughnessTextureAssetId", result.material.metallic_roughness_texture_id, error)) return false;
    const JsonValue* occlusion_texture = property(material, "occlusionTextureAssetId");
    if (occlusion_texture && !read_string(material, "occlusionTextureAssetId", result.material.occlusion_texture_id, error)) return false;
    const JsonValue* model_id = property(object, "modelAssetId");
    if (model_id && !read_string(object, "modelAssetId", result.model_asset_id, error)) return false;
    const JsonValue* material_id = property(object, "materialAssetId");
    if (material_id && !read_string(object, "materialAssetId", result.material_asset_id, error)) return false;
    return true;
}

void write_object(std::ostream& output, const SceneObject& object) {
    output << "    {\n      \"id\": " << object.id << ",\n      \"name\": ";
    write_string(output, object.name);
    output << ",\n      \"visible\": " << (object.visible ? "true" : "false") << ",\n      \"transform\": {\n        \"position\": ";
    write_vec3(output, object.transform.position);
    output << ",\n        \"rotation\": ";
    write_vec3(output, object.transform.rotation);
    output << ",\n        \"scale\": ";
    write_vec3(output, object.transform.scale);
    output << "\n      },\n      \"material\": {\n        \"baseColor\": ";
    write_vec3(output, object.material.base_color);
    output << ",\n        \"roughness\": " << object.material.roughness
           << ",\n        \"metallic\": " << object.material.metallic << ",\n        \"emission\": ";
    write_vec3(output, object.material.emission);
    output << ",\n        \"baseColorTextureAssetId\": ";
    write_string(output, object.material.base_color_texture_id);
    output << ",\n        \"normalTextureAssetId\": ";
    write_string(output, object.material.normal_texture_id);
    output << ",\n        \"metallicRoughnessTextureAssetId\": ";
    write_string(output, object.material.metallic_roughness_texture_id);
    output << ",\n        \"occlusionTextureAssetId\": ";
    write_string(output, object.material.occlusion_texture_id);
    output << "\n      },\n      \"modelAssetId\": ";
    write_string(output, object.model_asset_id);
    output << ",\n      \"materialAssetId\": ";
    write_string(output, object.material_asset_id);
    output << "\n    }";
}

} // namespace

Scene::Scene(std::string name) : name_(std::move(name)) {}

void Scene::set_name(std::string name) { name_ = std::move(name); }

SceneObject& Scene::create_object(std::string name) {
    if (next_id_ > 9007199254740991ULL) throw std::overflow_error("Scene object id space exhausted.");
    if (name.empty()) name = "Cube " + std::to_string(next_id_);
    SceneObject object{next_id_++, std::move(name), {}, {}, {}, {}, true};
    object.transform.position.y = 0.5f;
    objects_.push_back(std::move(object));
    return objects_.back();
}

SceneObject& Scene::duplicate_object(const std::uint64_t id) {
    const SceneObject* original = find_object(id);
    if (!original) return create_object();
    if (next_id_ > 9007199254740991ULL) throw std::overflow_error("Scene object id space exhausted.");
    SceneObject copy = *original;
    copy.id = next_id_++;
    copy.name += " Copy";
    copy.transform.position.x += 1.0f;
    objects_.push_back(std::move(copy));
    return objects_.back();
}

bool Scene::delete_object(const std::uint64_t id) {
    const auto found = std::find_if(objects_.begin(), objects_.end(), [id](const SceneObject& object) { return object.id == id; });
    if (found == objects_.end()) return false;
    objects_.erase(found);
    return true;
}

SceneObject* Scene::find_object(const std::uint64_t id) noexcept {
    const auto found = std::find_if(objects_.begin(), objects_.end(), [id](const SceneObject& object) { return object.id == id; });
    return found == objects_.end() ? nullptr : &*found;
}

const SceneObject* Scene::find_object(const std::uint64_t id) const noexcept {
    const auto found = std::find_if(objects_.begin(), objects_.end(), [id](const SceneObject& object) { return object.id == id; });
    return found == objects_.end() ? nullptr : &*found;
}

bool Scene::references_asset(const std::string_view asset_id) const noexcept {
    if (asset_id.empty()) return false;
    return std::any_of(objects_.begin(), objects_.end(), [asset_id](const SceneObject& object) {
        return std::string_view(object.model_asset_id) == asset_id || std::string_view(object.material_asset_id) == asset_id ||
            std::string_view(object.material.base_color_texture_id) == asset_id || std::string_view(object.material.normal_texture_id) == asset_id ||
            std::string_view(object.material.metallic_roughness_texture_id) == asset_id ||
            std::string_view(object.material.occlusion_texture_id) == asset_id;
    });
}

bool Scene::validate(std::string& error) const {
    error.clear();
    if (name_.empty()) { error = "Scene name cannot be empty."; return false; }
    if (name_.size() > 256) { error = "Scene name exceeds 256 UTF-8 bytes."; return false; }
    if (objects_.size() > 10000) { error = "Scene exceeds the 10,000 object limit."; return false; }
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(objects_.size());
    for (const SceneObject& object : objects_) {
        if (!object.id || object.id > 9007199254740991ULL) { error = "Scene object id must be a positive, JSON-safe integer."; return false; }
        if (!ids.insert(object.id).second) { error = "Scene contains duplicate object ids."; return false; }
        if (object.name.empty() || object.name.size() > 256) { error = "Object names must contain 1–256 UTF-8 bytes."; return false; }
        if (object.model_asset_id.size() > 256 || object.material_asset_id.size() > 256 ||
            object.material.base_color_texture_id.size() > 256 || object.material.normal_texture_id.size() > 256 ||
            object.material.metallic_roughness_texture_id.size() > 256 || object.material.occlusion_texture_id.size() > 256) {
            error = "Asset references cannot exceed 256 UTF-8 bytes.";
            return false;
        }
        const auto valid_transform = [](const Vec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
                std::abs(v.x) <= 1.0e6f && std::abs(v.y) <= 1.0e6f && std::abs(v.z) <= 1.0e6f;
        };
        if (!valid_transform(object.transform.position) || !valid_transform(object.transform.rotation) || !valid_transform(object.transform.scale)) {
            error = "Object transform values must be finite and within +/-1,000,000.";
            return false;
        }
        if (object.transform.scale.x <= 0 || object.transform.scale.y <= 0 || object.transform.scale.z <= 0) {
            error = "Object scale components must be greater than zero.";
            return false;
        }
        if (!valid_transform(object.material.base_color) || object.material.base_color.x < 0 || object.material.base_color.x > 1 ||
            object.material.base_color.y < 0 || object.material.base_color.y > 1 || object.material.base_color.z < 0 || object.material.base_color.z > 1 ||
            !std::isfinite(object.material.roughness) || object.material.roughness < 0 || object.material.roughness > 1 ||
            !std::isfinite(object.material.metallic) || object.material.metallic < 0 || object.material.metallic > 1) {
            error = "Material color, roughness, and metallic values must be within [0, 1].";
            return false;
        }
        if (!valid_transform(object.material.emission) || object.material.emission.x < 0 || object.material.emission.y < 0 ||
            object.material.emission.z < 0 || object.material.emission.x > 64 || object.material.emission.y > 64 || object.material.emission.z > 64) {
            error = "Material emission values must be finite and within [0, 64].";
            return false;
        }
    }
    return true;
}

bool Scene::save(const std::filesystem::path& path, std::string& error) const {
    if (path.empty()) { error = "Scene file path is empty."; return false; }
    if (!validate(error)) return false;
    std::ostringstream json;
    json << std::setprecision(9) << "{\n  \"format\": \"TogetherEngineScene\",\n  \"version\": " << format_version << ",\n  \"name\": ";
    write_string(json, name_);
    json << ",\n  \"objects\": [";
    for (std::size_t i = 0; i < objects_.size(); ++i) {
        json << (i ? ",\n" : "\n");
        write_object(json, objects_[i]);
    }
    if (!objects_.empty()) json << '\n';
    json << "  ]\n}\n";

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) { error = "Could not create temporary scene file: " + temporary.string(); return false; }
        const std::string contents = json.str();
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file) { error = "Could not write complete scene data."; return false; }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not replace scene file (Windows error " + std::to_string(GetLastError()) + ").";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    error.clear();
    return true;
}

bool Scene::load(const std::filesystem::path& path, Scene& output, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Could not open scene file: " + path.string(); return false; }
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (!size_error && file_size > 16 * 1024 * 1024) { error = "Scene file exceeds the 16 MiB limit."; return false; }
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (contents.size() > 16 * 1024 * 1024) { error = "Scene file exceeds the 16 MiB limit."; return false; }
    JsonValue root;
    if (!JsonParser(contents).parse(root, error)) return false;
    if (!std::holds_alternative<JsonValue::Object>(root.value)) { error = "Scene root must be a JSON object."; return false; }
    const auto& object = std::get<JsonValue::Object>(root.value);
    std::string format;
    if (!read_string(object, "format", format, error)) return false;
    if (format != "TogetherEngineScene") { error = "Unrecognized scene format."; return false; }
    double version{};
    if (!read_number(object, "version", version, error)) return false;
    if (version != format_version) { error = "Unsupported scene version " + std::to_string(version) + "."; return false; }
    std::string name;
    if (!read_string(object, "name", name, error)) return false;
    const JsonValue* objects_value = property(object, "objects");
    if (!objects_value || !std::holds_alternative<JsonValue::Array>(objects_value->value)) { error = "Scene requires an objects array."; return false; }
    const auto& values = std::get<JsonValue::Array>(objects_value->value);
    if (values.size() > 10000) { error = "Scene exceeds the 10,000 object limit."; return false; }
    Scene parsed(std::move(name));
    parsed.objects_.reserve(values.size());
    for (const JsonValue& value : values) {
        SceneObject object_value;
        if (!object_from_json(value, object_value, error)) return false;
        parsed.next_id_ = (std::max)(parsed.next_id_, object_value.id + 1);
        parsed.objects_.push_back(std::move(object_value));
    }
    if (!parsed.validate(error)) return false;
    output = std::move(parsed);
    error.clear();
    return true;
}

} // namespace together::scene
