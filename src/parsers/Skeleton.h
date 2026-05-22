#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct Bone {
    std::string  name;
    int          parent;   // -1 = root
    glm::vec3    world_pos;
};

struct Skeleton {
    std::vector<Bone>                    bones;
    std::vector<std::pair<int,int>>      lines;  // parent->child index pairs
};

// Parse BLACK_CAT.dat (skeleton) and pull bind-pose positions from the XBX.
// Returns nullptr on failure.
Skeleton* parse_skeleton(const std::string& skel_path, const std::string& xbx_path);
