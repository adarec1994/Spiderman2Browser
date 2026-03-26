#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

struct XBXSubmesh {
    std::string            mat_name;
    std::string            tex_name;       // best single guess (tex_candidates[0])
    std::vector<std::string> tex_candidates; // ordered: try each until one loads
    uint32_t               prim_type = 0;   // raw ptr+0x28 value from file
    std::vector<uint16_t>  raw_indices;     // original u16 index buffer (empty if none)
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t>  indices;       // triangle list
    std::vector<glm::vec4> bone_weights;  // normalized 0..1 (up to 4 influences)
    std::vector<glm::ivec4> bone_indices; // global bone index 0..59, -1 = unused
};

struct XBXModel {
    std::vector<XBXSubmesh> submeshes;
    std::string             filepath;
    // Bind-pose world matrices (row-major in XBX, stored transposed → column-major for GLM)
    // 60 matrices at 0x2f0 in the XBX file
    std::vector<glm::mat4> bind_pose; // [60]
};

XBXModel* parse_xbx(const std::string& filepath);