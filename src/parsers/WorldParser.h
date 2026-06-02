#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

// Named static object (manholes, lampposts, etc.).
struct WorldInstance {
    std::string name;        // e.g. "A01_S_MANHOLEB01"
    std::string asset_name;  // XBX asset stem, e.g. "s_manholeb000"
    glm::mat4   transform;   // world-space transform (GLM column-major)
};

// Mass-placed prop (street furniture, trees, signs).
struct WorldProp {
    int   type_idx; // index into WorldData::prop_types
    float yaw_deg;  // Y-axis rotation in degrees (0-359)
    float x, y, z; // world-space position
};

// Per-cell building-material atlas entry. Building models (smlego shader) use a
// shared master material name (s_blg_blgmaster / s_blg_pedmaster / s_blg_trm)
// that the engine resolves PER CELL to a set of real wall/edge textures, indexed
// by a per-face "slot" byte. The cell .dat embeds this slot->texture table.
struct WorldBlgTex {
    std::string master;   // "s_blg_blgmaster" | "s_blg_pedmaster" | "s_blg_trm"
    int         slot;     // slot id (0x78..0x89 etc.)
    std::string texture;  // real texture stem, e.g. "sd_blg_01sid1_res"
};

struct WorldData {
    std::string                source_path;
    std::vector<WorldInstance> instances;   // named static objects
    std::vector<std::string>   prop_types;  // e.g. "sa_awning_5b000"
    std::vector<WorldProp>     props;       // mass placements
    std::vector<WorldBlgTex>   blg_textures; // per-cell building atlas slot->texture
};

// Parse a Spider-Man 2 area .dat file.
// Returns nullptr on failure; caller owns the pointer.
WorldData* parse_world(const std::string& path);