#pragma once
// Convenience aliases and helpers around nlohmann/json.

#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <string>

namespace forge {

using Json = nlohmann::json;

namespace json_util {

/// Parse a JSON file. Throws std::runtime_error on failure.
inline Json parse_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open JSON file: " + path);
    }
    Json j;
    try {
        ifs >> j;
    } catch (const Json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + path + ": " + e.what());
    }
    return j;
}

/// Serialize to a pretty-printed string.
inline std::string to_pretty_string(const Json& j, int indent = 2) {
    return j.dump(indent);
}

}  // namespace json_util
}  // namespace forge
