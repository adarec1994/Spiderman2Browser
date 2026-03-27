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

struct WorldData {
    std::string                source_path;
    std::vector<WorldInstance> instances;   // named static objects
    std::vector<std::string>   prop_types;  // e.g. "sa_awning_5b000"
    std::vector<WorldProp>     props;       // mass placements
};

// Parse a Spider-Man 2 area .dat file.
// Returns nullptr on failure; caller owns the pointer.
WorldData* parse_world(const std::string& path);