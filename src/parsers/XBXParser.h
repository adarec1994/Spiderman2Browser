#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

struct XBXSubmesh {
    std::string            mat_name;
    std::string            shader_type;    // e.g. "smsimple", "smtranslucent", "character"
    std::string            tex_name;       // best single guess (tex_candidates[0])
    std::vector<std::string> tex_candidates; // ordered: try each until one loads
    uint32_t               prim_type = 0;   // raw ptr+0x28 value from file
    bool                   from_pushbuffer = false; // sourced from +0x38 pushbuffer path
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

// primary_geom_only: for single-model viewing, keep only the main body GEO
// container (the one with the most vertices). Some character XBXes carry extra
// GEO containers (auxiliary LOD/objects stored off to the side) that otherwise
// render as duplicate "extra models". World/terrain loading must pass false so
// every mesh section is kept.
XBXModel* parse_xbx(const std::string& filepath, bool primary_geom_only = false);

// Locate the bind-pose matrix array inside an XBX blob. The array is NOT at a
// fixed offset (it sits after a variable-size header/chunk+material table, so it
// ranges ~0x1d0..0x350 across characters). It is the first run of >=4 affine
// 4x4 float matrices (column-major, bottom row 0,0,0,1) whose first matrix is the
// root at the origin. Returns the byte offset (0 if not found) and, if out_count
// is non-null, the number of consecutive matrices in the run.
size_t xbx_find_bind_matrix_base(const uint8_t* d, size_t sz, int* out_count = nullptr);
