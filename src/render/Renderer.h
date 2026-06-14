#pragma once
#include "XBXParser.h"
#include "Skeleton.h"
#include "Camera.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

struct GPUMesh {
    std::string  mat_name;
    unsigned int tex_id    = 0;
    unsigned int vao       = 0;
    unsigned int vbo       = 0;  
    unsigned int bone_vbo  = 0;  
    unsigned int ibo       = 0;
    int          n_indices = 0;
    bool         translucent = false;
    bool         is_blg = false; 

    void draw() const;
    void release();
};

struct GPUModel {
    std::vector<GPUMesh> meshes;
    glm::vec3            center{0};
    float                scale = 1.f;
    float                bb_ymin = 0.f, bb_ymax = 0.f; 
    std::string          filepath; 

    void draw() const;
    void release();
    void update_mesh_indices(int mesh_idx, const std::vector<uint32_t>& tris);
};








struct InstancedModel {
    GPUModel*               model = nullptr;  
    unsigned int            tex_override = 0; 
    std::vector<glm::mat4>  xforms;           
    unsigned int            inst_vbo = 0;     
    void release();
};



struct WorldPlacement {
    GPUModel*    model = nullptr;
    glm::mat4    xform{1.f};
    unsigned int tex_override = 0;
};

struct InstancedWorld {
    std::vector<InstancedModel> models;
    long long                   total_instances = 0;
    long long                   total_draws     = 0; 
    void release();
};

struct GPUSkeleton {
    unsigned int vao    = 0, vbo    = 0;
    unsigned int pt_vao = 0, pt_vbo = 0;
    int          n      = 0;
    int          n_pts  = 0;

    void build(const Skeleton& sk);
    void draw()            const;
    void draw_joints()     const;
    void draw_joint(int i) const;
    void release();
};

class Renderer {
public:
    bool  wireframe   = false;
    bool  show_grid   = true;
    bool  show_skel   = true;
    bool  show_uv     = false;
    int   sel_bone    = -1;
    int   sel_submesh = -1;
    float model_rot_y = 0.f;

    void init();
    void shutdown();

    GPUModel*    upload_model(const XBXModel* model);
    GPUSkeleton* upload_skeleton(const Skeleton* sk);

    
    
    void set_bone_matrices(const glm::mat4* mats, int count);

    void draw_scene(const Camera& cam, int vp_x, int vp_w, int vp_h,
                    const GPUModel* model, const GPUSkeleton* skel);

    
    
    
    void draw_world_instances(const Camera& cam, int vp_x, int vp_w, int vp_h,
                              const std::vector<std::pair<GPUModel*, glm::mat4>>& instances);

    
    
    
    InstancedWorld build_instanced_world(
        const std::vector<WorldPlacement>& instances);

    
    
    void draw_instanced_world(const Camera& cam, int vp_x, int vp_w, int vp_h,
                              const InstancedWorld& iw);

private:
    unsigned int m_shader   = 0;
    unsigned int m_grid_vao = 0, m_grid_vbo = 0;
    int          m_grid_n   = 0;

    
    glm::mat4    m_bone_mats[60];
    bool         m_bones_dirty = true;

    int  uloc(const char* name);
    void build_grid(int half, float step);
};