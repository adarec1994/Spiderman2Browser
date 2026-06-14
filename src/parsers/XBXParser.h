#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

struct XBXSubmesh {
    std::string            mat_name;
    std::string            shader_type;    
    std::string            tex_name;       
    std::vector<std::string> tex_candidates; 
    uint32_t               prim_type = 0;   
    bool                   from_pushbuffer = false; 
    std::vector<uint16_t>  raw_indices;     
    std::vector<glm::vec3> positions;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t>  indices;       
    std::vector<glm::vec4> bone_weights;  
    std::vector<glm::ivec4> bone_indices; 
};

struct XBXModel {
    std::vector<XBXSubmesh> submeshes;
    std::string             filepath;
    
    
    std::vector<glm::mat4> bind_pose; 
};






XBXModel* parse_xbx(const std::string& filepath, bool primary_geom_only = false);







size_t xbx_find_bind_matrix_base(const uint8_t* d, size_t sz, int* out_count = nullptr);
