#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

struct XBXSubmesh {
    std::string          mat_name;
    std::string          tex_name;   // hint for texture lookup
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t>  indices;  // triangle list (already expanded from strip)
};

struct XBXModel {
    std::vector<XBXSubmesh> submeshes;
    std::string             filepath;
};

// Returns nullptr if file is not a valid XBXM or contains no geometry.
XBXModel* parse_xbx(const std::string& filepath);
