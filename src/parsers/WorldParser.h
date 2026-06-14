#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>


struct WorldInstance {
    std::string name;        
    std::string asset_name;  
    glm::mat4   transform;   
};


struct WorldProp {
    int   type_idx; 
    float yaw_deg;  
    float x, y, z; 
    
    
    
    float height = 0.f;
    
    
    
    int   slot = -1;
};





struct WorldBlgTex {
    std::string master;   
    int         slot;     
    std::string texture;  
};

struct WorldData {
    std::string                source_path;
    std::vector<WorldInstance> instances;   
    std::vector<std::string>   prop_types;  
    std::vector<WorldProp>     props;       
    std::vector<WorldBlgTex>   blg_textures; 
};



WorldData* parse_world(const std::string& path);