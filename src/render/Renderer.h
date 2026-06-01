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
    unsigned int vbo       = 0;  // positions + uvs
    unsigned int bone_vbo  = 0;  // bone idx + wt (vec4+vec4)
    unsigned int ibo       = 0;
    int          n_indices = 0;
    bool         translucent = false;

    void draw() const;
    void release();
};

struct GPUModel {
    std::vector<GPUMesh> meshes;
    glm::vec3            center{0};
    float                scale = 1.f;
    std::string          filepath; // source XBX path

    void draw() const;
    void release();
    void update_mesh_indices(int mesh_idx, const std::vector<uint32_t>& tris);
};

// Instanced world: each UNIQUE model keeps its geometry ONCE and is drawn for all
// its placements with a single instanced draw call (one per submesh of the unique
// model). A model appearing 150× costs 150 mat4s in a small per-instance buffer
// instead of 150 copies of its vertices. This collapses the ~30k per-instance
// draws of a full city by the instance/unique ratio (~13×), with near-zero build
// cost and minimal extra memory — and removes the per-instance frustum cull that
// made big terrain tiles vanish up close.
struct InstancedModel {
    GPUModel*               model = nullptr;  // owned by m_world_gpu_cache (not freed here)
    std::vector<glm::mat4>  xforms;           // one world matrix per placement
    unsigned int            inst_vbo = 0;     // GL buffer holding xforms (mat4 each)
    void release();
};

struct InstancedWorld {
    std::vector<InstancedModel> models;
    long long                   total_instances = 0;
    long long                   total_draws     = 0; // submesh-draws issued per frame
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

    // Upload 60 skinning matrices (current_pose * inv_bind_pose).
    // Call this whenever bones change. Pass nullptr to reset to identity (bind pose).
    void set_bone_matrices(const glm::mat4* mats, int count);

    void draw_scene(const Camera& cam, int vp_x, int vp_w, int vp_h,
                    const GPUModel* model, const GPUSkeleton* skel);

    // Draw all world instances in a single pass.
    // Each transform should already have the model's S*T normalization pre-baked in.
    // Clears the viewport, draws all meshes, then the grid.
    void draw_world_instances(const Camera& cam, int vp_x, int vp_w, int vp_h,
                              const std::vector<std::pair<GPUModel*, glm::mat4>>& instances);

    // Build an instanced world: group placements by unique GPUModel and upload a
    // per-instance transform buffer for each. The model's geometry is shared
    // (not copied), so this is cheap to build and memory-light.
    InstancedWorld build_instanced_world(
        const std::vector<std::pair<GPUModel*, glm::mat4>>& instances);

    // Draw a pre-built instanced world (one instanced draw per unique submesh).
    // Clears viewport + grid.
    void draw_instanced_world(const Camera& cam, int vp_x, int vp_w, int vp_h,
                              const InstancedWorld& iw);

private:
    unsigned int m_shader   = 0;
    unsigned int m_grid_vao = 0, m_grid_vbo = 0;
    int          m_grid_n   = 0;

    // Bone matrix UBO / uniform cache
    glm::mat4    m_bone_mats[60];
    bool         m_bones_dirty = true;

    int  uloc(const char* name);
    void build_grid(int half, float step);
};