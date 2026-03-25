#pragma once
#include "XBXParser.h"
#include "Skeleton.h"
#include "Camera.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

// Per-submesh GPU data
struct GPUMesh {
    std::string  mat_name;
    unsigned int tex_id    = 0;
    unsigned int vao       = 0;
    unsigned int vbo       = 0;
    unsigned int ibo       = 0;
    int          n_indices = 0;

    void draw() const;
    void release();
};

// All submeshes for one model
struct GPUModel {
    std::vector<GPUMesh> meshes;
    glm::vec3            center{0};
    float                scale = 1.f;

    void draw() const;
    void release();
};

// Skeleton line buffer
struct GPUSkeleton {
    unsigned int vao    = 0, vbo    = 0;  // bone lines
    unsigned int pt_vao = 0, pt_vbo = 0;  // joint points
    int          n      = 0;              // line vertex count
    int          n_pts  = 0;              // joint count

    void build(const Skeleton& sk);
    void draw()           const;   // bone lines
    void draw_joints()    const;   // all joint dots
    void draw_joint(int i) const;  // single joint (selected)
    void release();
};

class Renderer {
public:
    bool  wireframe    = false;
    bool  show_grid    = true;
    bool  show_skel    = true;
    int   sel_bone     = -1;    // highlighted bone index (-1 = none)
    float model_rot_y  = 0.f;  // whole-model Y rotation (radians)

    void init();                  // compile shaders, build grid
    void shutdown();

    // Upload model/skeleton to GPU. Caller owns the CPU-side objects.
    GPUModel*    upload_model(const XBXModel* model);
    GPUSkeleton* upload_skeleton(const Skeleton* sk);

    // Draw the full 3-D scene
    void draw_scene(const Camera& cam, int vp_x, int vp_w, int vp_h,
                    const GPUModel* model, const GPUSkeleton* skel);

private:
    unsigned int m_shader = 0;
    unsigned int m_grid_vao = 0;
    unsigned int m_grid_vbo = 0;
    int          m_grid_n   = 0;

    int uloc(const char* name);
    void build_grid(int half, float step);
};