#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct Bone {
    std::string  name;
    int          parent;   
    glm::vec3    world_pos;
};

struct Skeleton {
    std::vector<Bone>                    bones;
    std::vector<std::pair<int,int>>      lines;  
};



Skeleton* parse_skeleton(const std::string& skel_path, const std::string& xbx_path);
