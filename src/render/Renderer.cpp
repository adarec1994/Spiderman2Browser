#include "Renderer.h"
#include "Texture.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <utility>
#include <unordered_map>

namespace fs = std::filesystem;

// ── Shaders ───────────────────────────────────────────────
static const char* VERT_SRC = R"(
#version 330 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec2  aUV;
layout(location=2) in vec4  aBoneIdx;
layout(location=3) in vec4  aBoneWt;
layout(location=4) in mat4  aInstModel;   // per-instance world matrix (loc 4,5,6,7)

uniform mat4  uMVP;
uniform mat4  uModel;
uniform float uPointSize;
uniform mat4  uBones[60];
uniform bool  uSkinned;
uniform bool  uInstanced;   // world instancing: use aInstModel + uVP
uniform mat4  uVP;          // view*proj (instanced path; vertices are in local space)

out vec2 vUV;

void main(){
    vec4 pos = vec4(aPos, 1.0);
    if (uSkinned) {
        vec4 skinned = vec4(0.0);
        float wsum = 0.0;
        for (int i = 0; i < 4; i++) {
            float w = aBoneWt[i];
            if (w < 0.001) continue;
            int   bi = int(aBoneIdx[i]);
            skinned += w * (uBones[bi] * pos);
            wsum += w;
        }
        if (wsum > 0.001) pos = skinned / wsum;
    }
    gl_Position  = uInstanced ? (uVP * aInstModel * pos) : (uMVP * pos);
    gl_PointSize = uPointSize;
    vUV = aUV;
}
)";

static const char* FRAG_SRC = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform bool uHasTex;
uniform bool uWire;
uniform bool uShowUV;
uniform bool uTranslucent;
uniform vec3 uWireCol;
out vec4 fragColor;

void main(){
    if(uWire)   { fragColor=vec4(uWireCol,1.0); return; }
    if(uShowUV) { fragColor=vec4(fract(vUV.x), fract(vUV.y), 0.2, 1.0); return; }

    vec4 texel = uHasTex ? texture(uTex, vUV) : vec4(0.60, 0.62, 0.65, 1.0);
    float alpha = uTranslucent ? texel.a : 1.0;
    if (uTranslucent && alpha < 0.02) discard;

    // Unlit: show the texture as-authored, gamma-expanded for a linear display.
    vec3 base = pow(max(texel.rgb, vec3(0.0001)), vec3(1.0 / 2.2));
    fragColor = vec4(base, alpha);
}
)";

static unsigned int compile_shader(const char* src, GLenum type) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

// ── GPUMesh ───────────────────────────────────────────────
void GPUMesh::draw() const {
    glBindVertexArray(vao);
    if (tex_id) { glBindTexture(GL_TEXTURE_2D,tex_id); glEnable(GL_TEXTURE_2D); }
    else          glDisable(GL_TEXTURE_2D);
    glDrawElements(GL_TRIANGLES, n_indices, GL_UNSIGNED_INT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}
void GPUMesh::release() {
    glDeleteVertexArrays(1,&vao); glDeleteBuffers(1,&vbo);
    glDeleteBuffers(1,&bone_vbo); glDeleteBuffers(1,&ibo);
    vao=vbo=bone_vbo=ibo=0;
}

// ── GPUModel ──────────────────────────────────────────────
void GPUModel::draw() const { for (auto& m:meshes) m.draw(); }
void GPUModel::release()    { for (auto& m:meshes) m.release(); }

// ── GPUSkeleton ───────────────────────────────────────────
void GPUSkeleton::build(const Skeleton& sk) {
    std::vector<float> line_v, pt_v;
    for (auto& [a,b]:sk.lines) {
        auto& pa=sk.bones[a].world_pos; auto& pb=sk.bones[b].world_pos;
        line_v.insert(line_v.end(),{pa.x,pa.y,pa.z,pb.x,pb.y,pb.z});
    }
    n = (int)sk.lines.size()*2;
    for (auto& bone:sk.bones)
        pt_v.insert(pt_v.end(),{bone.world_pos.x,bone.world_pos.y,bone.world_pos.z});
    n_pts = (int)sk.bones.size();

    auto upload = [](unsigned int& va, unsigned int& vb, const std::vector<float>& data) {
        if (!va) { glGenVertexArrays(1,&va); glGenBuffers(1,&vb); }
        glBindVertexArray(va);
        glBindBuffer(GL_ARRAY_BUFFER,vb);
        glBufferData(GL_ARRAY_BUFFER,data.size()*4,data.data(),GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,12,nullptr);
        glBindVertexArray(0);
    };
    upload(vao,vbo,line_v);
    upload(pt_vao,pt_vbo,pt_v);
}
void GPUSkeleton::draw()       const { glBindVertexArray(vao);    glDrawArrays(GL_LINES,0,n);      glBindVertexArray(0); }
void GPUSkeleton::draw_joints() const { glBindVertexArray(pt_vao); glDrawArrays(GL_POINTS,0,n_pts); glBindVertexArray(0); }
void GPUSkeleton::draw_joint(int i) const {
    if (i<0||i>=n_pts) return;
    glBindVertexArray(pt_vao); glDrawArrays(GL_POINTS,i,1); glBindVertexArray(0);
}
void GPUSkeleton::release() {
    glDeleteVertexArrays(1,&vao);    glDeleteBuffers(1,&vbo);
    glDeleteVertexArrays(1,&pt_vao); glDeleteBuffers(1,&pt_vbo);
    vao=vbo=pt_vao=pt_vbo=0;
}

// ── Renderer ─────────────────────────────────────────────
void Renderer::init() {
    unsigned int vs = compile_shader(VERT_SRC, GL_VERTEX_SHADER);
    unsigned int fs = compile_shader(FRAG_SRC, GL_FRAGMENT_SHADER);
    m_shader = glCreateProgram();
    glAttachShader(m_shader,vs); glAttachShader(m_shader,fs);
    glLinkProgram(m_shader);
    glDeleteShader(vs); glDeleteShader(fs);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.10f,0.10f,0.13f,1.f);

    // Init bone matrices to identity
    for (auto& m : m_bone_mats) m = glm::mat4(1.f);
    m_bones_dirty = true;

    build_grid(20, 0.1f);
}

void Renderer::shutdown() {
    glDeleteProgram(m_shader);
    glDeleteVertexArrays(1,&m_grid_vao); glDeleteBuffers(1,&m_grid_vbo);
}

int Renderer::uloc(const char* n) { return glGetUniformLocation(m_shader, n); }

void Renderer::set_bone_matrices(const glm::mat4* mats, int count) {
    int n = std::min(count, 60);
    if (mats) memcpy(m_bone_mats, mats, n*sizeof(glm::mat4));
    else      for (int i=0;i<60;++i) m_bone_mats[i]=glm::mat4(1.f);
    m_bones_dirty = true;
}

void Renderer::build_grid(int half, float step) {
    std::vector<float> lines;
    float t = half*step;
    for (int i=-half; i<=half; ++i) {
        float x=i*step;
        lines.insert(lines.end(),{x,-0.001f,-t, x,-0.001f,t});
        lines.insert(lines.end(),{-t,-0.001f,x, t,-0.001f,x});
    }
    m_grid_n = (int)lines.size()/3;
    glGenVertexArrays(1,&m_grid_vao); glGenBuffers(1,&m_grid_vbo);
    glBindVertexArray(m_grid_vao);
    glBindBuffer(GL_ARRAY_BUFFER,m_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER,lines.size()*4,lines.data(),GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,12,nullptr);
    glBindVertexArray(0);
}

GPUModel* Renderer::upload_model(const XBXModel* model) {
    if (!model) return nullptr;
    std::string dir = fs::path(model->filepath).parent_path().string();
    auto* gm = new GPUModel();
    gm->filepath = model->filepath;

    glm::vec3 mn(1e9f), mx(-1e9f);
    for (auto& sm:model->submeshes) for (auto& p:sm.positions) { mn=glm::min(mn,p); mx=glm::max(mx,p); }
    gm->center = (mn+mx)*0.5f;
    gm->scale  = std::max(std::max(mx.x-mn.x, mx.y-mn.y), std::max(mx.z-mn.z, 1e-6f));

    for (auto& sm : model->submeshes) {
        GPUMesh m;
        m.mat_name    = sm.mat_name;
        m.n_indices   = (int)sm.indices.size();
        m.tex_id      = find_texture(sm.tex_candidates, dir);
        m.translucent = (sm.shader_type.find("translucent") != std::string::npos ||
                         sm.shader_type.find("glass")       != std::string::npos ||
                         sm.shader_type.find("fxenv")       != std::string::npos);
        // Material names ending in "_a" indicate alpha channel (e.g. "lamplite1_a")
        if (!m.translucent && sm.mat_name.size() >= 2) {
            auto tail = sm.mat_name.substr(sm.mat_name.size() - 2);
            if (tail == "_a" || tail == "_A") m.translucent = true;
        }

        // Interleave XYZ + UV
        std::vector<float> vdata;
        vdata.reserve(sm.positions.size()*5);
        for (size_t i=0; i<sm.positions.size(); ++i) {
            vdata.push_back(sm.positions[i].x); vdata.push_back(sm.positions[i].y); vdata.push_back(sm.positions[i].z);
            vdata.push_back(sm.uvs[i].x);       vdata.push_back(sm.uvs[i].y);
        }

        // Bone data: vec4 indices (as float) + vec4 weights
        std::vector<float> bdata;
        bdata.reserve(sm.positions.size()*8);
        for (size_t i=0; i<sm.positions.size(); ++i) {
            bdata.push_back((float)sm.bone_indices[i].x); bdata.push_back((float)sm.bone_indices[i].y);
            bdata.push_back((float)sm.bone_indices[i].z); bdata.push_back((float)sm.bone_indices[i].w);
            bdata.push_back(sm.bone_weights[i].x); bdata.push_back(sm.bone_weights[i].y);
            bdata.push_back(sm.bone_weights[i].z); bdata.push_back(sm.bone_weights[i].w);
        }

        glGenVertexArrays(1,&m.vao); glGenBuffers(1,&m.vbo); glGenBuffers(1,&m.bone_vbo); glGenBuffers(1,&m.ibo);
        glBindVertexArray(m.vao);

        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, vdata.size()*4, vdata.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,20,(void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,20,(void*)12);

        glBindBuffer(GL_ARRAY_BUFFER, m.bone_vbo);
        glBufferData(GL_ARRAY_BUFFER, bdata.size()*4, bdata.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,32,(void*)0);   // bone idx
        glEnableVertexAttribArray(3); glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,32,(void*)16);  // bone wt

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sm.indices.size()*4, sm.indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);

        gm->meshes.push_back(std::move(m));
    }
    return gm;
}

void GPUModel::update_mesh_indices(int mesh_idx, const std::vector<uint32_t>& tris) {
    if (mesh_idx < 0 || mesh_idx >= (int)meshes.size()) return;
    GPUMesh& m = meshes[mesh_idx];
    m.n_indices = (int)tris.size();
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, tris.size()*4, tris.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

GPUSkeleton* Renderer::upload_skeleton(const Skeleton* sk) {
    if (!sk) return nullptr;
    auto* gs = new GPUSkeleton();
    gs->build(*sk);
    return gs;
}

void Renderer::draw_scene(const Camera& cam, int vp_x, int vp_w, int vp_h,
                          const GPUModel* model, const GPUSkeleton* skel) {
    glViewport(vp_x,0,vp_w,vp_h);
    glScissor(vp_x,0,vp_w,vp_h);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    if (!model) { glDisable(GL_SCISSOR_TEST); return; }

    float aspect = (float)vp_w / (float)std::max(vp_h,1);

    glm::mat4 S  = glm::scale(glm::mat4(1), glm::vec3(1.f/model->scale));
    glm::mat4 T  = glm::translate(glm::mat4(1), -model->center);
    glm::mat4 Ry = glm::rotate(glm::mat4(1), model_rot_y, glm::vec3(0,1,0));
    glm::mat4 MVP = cam.proj(aspect) * cam.view() * S * Ry * T;
    glm::mat4 M   = S * Ry * T;

    glUseProgram(m_shader);
    glUniform1i(uloc("uInstanced"), 0);
    glUniformMatrix4fv(uloc("uMVP"),  1, GL_FALSE, glm::value_ptr(MVP));
    glUniformMatrix4fv(uloc("uModel"),1, GL_FALSE, glm::value_ptr(M));
    glUniform1i(uloc("uTex"),0);
    glUniform1i(uloc("uWire"),0);
    glUniform1i(uloc("uShowUV"), show_uv ? 1 : 0);
    glUniform1f(uloc("uPointSize"),5.f);
    glActiveTexture(GL_TEXTURE0);

    // Upload bone matrices once per frame
    if (m_bones_dirty) {
        glUniformMatrix4fv(uloc("uBones"), 60, GL_FALSE, glm::value_ptr(m_bone_mats[0]));
        m_bones_dirty = false;
    } else {
        glUniformMatrix4fv(uloc("uBones"), 60, GL_FALSE, glm::value_ptr(m_bone_mats[0]));
    }

    // Check if any bone matrix differs from identity
    bool is_skinned = false;
    for (int i=0;i<60;++i) if (m_bone_mats[i] != glm::mat4(1.f)) { is_skinned=true; break; }
    glUniform1i(uloc("uSkinned"), is_skinned ? 1 : 0);

    // Solid mesh
    for (auto& mesh : model->meshes) {
        glUniform1i(uloc("uHasTex"), mesh.tex_id ? 1 : 0);
        glUniform1i(uloc("uTranslucent"), mesh.translucent ? 1 : 0);
        mesh.draw();
    }

    // Selected submesh — green overlay (drawn before optional wireframe so wireframe is on top)
    if (sel_submesh >= 0 && sel_submesh < (int)model->meshes.size()) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glUniform1i(uloc("uWire"), 1);
        glUniform3f(uloc("uWireCol"), 0.1f, 1.0f, 0.3f);
        glLineWidth(2.5f);
        glUniform1i(uloc("uHasTex"), 0);
        model->meshes[sel_submesh].draw();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glLineWidth(1.f);
    }

    // Wireframe
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK,GL_LINE);
        glUniform1i(uloc("uWire"),1);
        glUniform3f(uloc("uWireCol"),0.2f,0.9f,0.4f);
        glLineWidth(1.2f);
        for (auto& mesh:model->meshes) mesh.draw();
        glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    }

    // Skeleton — always on top
    if (show_skel && skel && skel->n > 0) {
        glm::mat4 skelMVP = cam.proj(aspect) * cam.view() * S * T; // no Ry — bones in world space
        glUniformMatrix4fv(uloc("uMVP"),1,GL_FALSE,glm::value_ptr(skelMVP));
        glUniform1i(uloc("uWire"),1);
        glUniform1i(uloc("uSkinned"),0); // skeleton uses raw positions

        glDisable(GL_DEPTH_TEST);

        glLineWidth(1.5f);
        glUniform3f(uloc("uWireCol"),0.45f,0.65f,1.0f);
        skel->draw();
        glLineWidth(1.f);

        glEnable(GL_PROGRAM_POINT_SIZE);

        glUniform1f(uloc("uPointSize"),5.f);
        glUniform3f(uloc("uWireCol"),0.8f,0.8f,0.8f);
        skel->draw_joints();

        if (sel_bone >= 0) {
            glUniform1f(uloc("uPointSize"),10.f);
            glUniform3f(uloc("uWireCol"),1.f,0.55f,0.05f);
            skel->draw_joint(sel_bone);
        }
        glDisable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_DEPTH_TEST);
        glUniformMatrix4fv(uloc("uMVP"),1,GL_FALSE,glm::value_ptr(MVP));
    }

    // Grid
    if (show_grid) {
        glm::mat4 gridMVP = cam.proj(aspect)*cam.view();
        glUniformMatrix4fv(uloc("uMVP"),1,GL_FALSE,glm::value_ptr(gridMVP));
        glUniform1i(uloc("uWire"),1);
        glUniform1i(uloc("uSkinned"),0);
        glUniform3f(uloc("uWireCol"),0.22f,0.22f,0.26f);
        glBindVertexArray(m_grid_vao);
        glDrawArrays(GL_LINES,0,m_grid_n);
        glBindVertexArray(0);
    }

    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}

// ── World instance rendering ──────────────────────────────────────────────────
// Reuses the same shader/mesh draw path as draw_scene but:
//   - no per-model scale/center normalisation (pre-baked into transforms)
//   - no skeleton, no submesh highlight, no model rotation
//   - draws all instances then the grid once at the end
void Renderer::draw_world_instances(const Camera& cam, int vp_x, int vp_w, int vp_h,
                                    const std::vector<std::pair<GPUModel*, glm::mat4>>& instances) {
    glViewport(vp_x, 0, vp_w, vp_h);
    glScissor (vp_x, 0, vp_w, vp_h);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)vp_w / (float)std::max(vp_h, 1);
    glm::mat4 VP = cam.proj(aspect) * cam.view();

    // Extract frustum planes from VP for culling (6 planes, world space)
    // Gribb/Hartmann method
    glm::mat4 VPt = glm::transpose(VP);
    glm::vec4 planes[6] = {
        VPt[3] + VPt[0], // left
        VPt[3] - VPt[0], // right
        VPt[3] + VPt[1], // bottom
        VPt[3] - VPt[1], // top
        VPt[3] + VPt[2], // near
        VPt[3] - VPt[2], // far
    };
    // Normalise just the xyz for distance test
    for (auto& p : planes) {
        float len = glm::length(glm::vec3(p));
        if (len > 0.f) p /= len;
    }

    auto in_frustum = [&](glm::vec3 pos, float radius) {
        for (auto& p : planes)
            if (p.x*pos.x + p.y*pos.y + p.z*pos.z + p.w < -radius)
                return false;
        return true;
    };

    glUseProgram(m_shader);
    glUniform1i(uloc("uTex"),     0);
    glUniform1i(uloc("uWire"),    0);
    glUniform1i(uloc("uShowUV"),  0);
    glUniform1i(uloc("uSkinned"), 0);
    glUniform1i(uloc("uInstanced"), 0);
    glUniform1f(uloc("uPointSize"), 1.f);

    // Identity bones uploaded once
    static glm::mat4 id_bones[60];
    static bool id_init = false;
    if (!id_init) { for (auto& m : id_bones) m = glm::mat4(1.f); id_init = true; }
    glUniformMatrix4fv(uloc("uBones"), 60, GL_FALSE, glm::value_ptr(id_bones[0]));

    glActiveTexture(GL_TEXTURE0);

    unsigned int last_tex = ~0u;
    int drawn = 0, culled = 0;
    for (auto& [model, xform] : instances) {
        if (!model) continue;

        // Frustum cull: bounding sphere from xform translation + model scale
        glm::vec3 pos = glm::vec3(xform[3]);
        float radius  = model->scale * 2.0f; // conservative
        if (!in_frustum(pos, radius)) { ++culled; continue; }
        ++drawn;
        glm::mat4 MVP = VP * xform;
        glUniformMatrix4fv(uloc("uMVP"),   1, GL_FALSE, glm::value_ptr(MVP));
        glUniformMatrix4fv(uloc("uModel"), 1, GL_FALSE, glm::value_ptr(xform));

        for (auto& mesh : model->meshes) {
            glUniform1i(uloc("uHasTex"), mesh.tex_id ? 1 : 0);
            glUniform1i(uloc("uTranslucent"), mesh.translucent ? 1 : 0);
            if (mesh.tex_id != last_tex) {
                glBindTexture(GL_TEXTURE_2D, mesh.tex_id);
                last_tex = mesh.tex_id;
            }
            mesh.draw();
        }

        if (wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glUniform1i(uloc("uWire"), 1);
            glUniform3f(uloc("uWireCol"), 0.2f, 0.9f, 0.4f);
            glLineWidth(1.0f);
            for (auto& mesh : model->meshes) mesh.draw();
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glUniform1i(uloc("uWire"), 0);
        }
    }

    // Grid
    if (show_grid) {
        glm::mat4 gridMVP = VP;
        glUniformMatrix4fv(uloc("uMVP"),   1, GL_FALSE, glm::value_ptr(gridMVP));
        glUniform1i(uloc("uWire"),    1);
        glUniform1i(uloc("uSkinned"), 0);
        glUniform3f(uloc("uWireCol"), 0.22f, 0.22f, 0.26f);
        glBindVertexArray(m_grid_vao);
        glDrawArrays(GL_LINES, 0, m_grid_n);
        glBindVertexArray(0);
    }

    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}

// ── Instanced world ───────────────────────────────────────
void InstancedModel::release() {
    if (inst_vbo) glDeleteBuffers(1, &inst_vbo);
    inst_vbo = 0;
    xforms.clear();
    model = nullptr;   // not owned here
}
void InstancedWorld::release() {
    for (auto& im : models) im.release();
    models.clear();
    total_instances = total_draws = 0;
}

// Attach the per-instance mat4 stream (locations 4..7) to a model's submesh VAOs.
// A mat4 vertex attribute occupies 4 consecutive vec4 slots, each advancing once
// per instance (glVertexAttribDivisor = 1).
static void attach_instance_buffer(GPUModel* model, unsigned int inst_vbo) {
    for (auto& mesh : model->meshes) {
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
        for (int c = 0; c < 4; ++c) {
            glEnableVertexAttribArray(4 + c);
            glVertexAttribPointer(4 + c, 4, GL_FLOAT, GL_FALSE,
                                  (GLsizei)sizeof(glm::mat4),
                                  (void*)(uintptr_t)(c * sizeof(glm::vec4)));
            glVertexAttribDivisor(4 + c, 1);   // advance once per instance
        }
        glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

InstancedWorld Renderer::build_instanced_world(
        const std::vector<std::pair<GPUModel*, glm::mat4>>& instances) {
    InstancedWorld iw;

    // Group placements by unique GPUModel pointer (the same model is shared across
    // all its placements via m_world_gpu_cache).
    std::unordered_map<GPUModel*, size_t> index_of;   // model -> slot in iw.models
    for (auto& [model, xform] : instances) {
        if (!model) continue;
        auto it = index_of.find(model);
        if (it == index_of.end()) {
            index_of[model] = iw.models.size();
            InstancedModel im; im.model = model;
            im.xforms.push_back(xform);
            iw.models.push_back(std::move(im));
        } else {
            iw.models[it->second].xforms.push_back(xform);
        }
    }

    // Upload each model's per-instance transform buffer and wire it to its VAOs.
    for (auto& im : iw.models) {
        if (!im.model || im.xforms.empty()) continue;
        glGenBuffers(1, &im.inst_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, im.inst_vbo);
        glBufferData(GL_ARRAY_BUFFER, im.xforms.size() * sizeof(glm::mat4),
                     im.xforms.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        attach_instance_buffer(im.model, im.inst_vbo);
        iw.total_instances += (long long)im.xforms.size();
        iw.total_draws     += (long long)im.model->meshes.size();   // one instanced draw per submesh
    }

    return iw;
}

void Renderer::draw_instanced_world(const Camera& cam, int vp_x, int vp_w, int vp_h,
                                    const InstancedWorld& iw) {
    glViewport(vp_x, 0, vp_w, vp_h);
    glScissor (vp_x, 0, vp_w, vp_h);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)vp_w / (float)std::max(vp_h, 1);
    glm::mat4 VP = cam.proj(aspect) * cam.view();

    glUseProgram(m_shader);
    glUniform1i(uloc("uTex"),     0);
    glUniform1i(uloc("uWire"),    0);
    glUniform1i(uloc("uShowUV"),  0);
    glUniform1i(uloc("uSkinned"), 0);
    glUniform1i(uloc("uInstanced"), 1);                // use aInstModel + uVP path
    glUniform1f(uloc("uPointSize"), 1.f);
    glUniformMatrix4fv(uloc("uVP"), 1, GL_FALSE, glm::value_ptr(VP));

    glActiveTexture(GL_TEXTURE0);

    auto draw_pass = [&](bool translucent_pass) {
        unsigned int last_tex = ~0u;
        for (auto& im : iw.models) {
            if (!im.model || im.xforms.empty()) continue;
            const GLsizei ninst = (GLsizei)im.xforms.size();
            for (auto& mesh : im.model->meshes) {
                if (mesh.translucent != translucent_pass) continue;
                glUniform1i(uloc("uHasTex"), mesh.tex_id ? 1 : 0);
                glUniform1i(uloc("uTranslucent"), mesh.translucent ? 1 : 0);
                if (mesh.tex_id != last_tex) { glBindTexture(GL_TEXTURE_2D, mesh.tex_id); last_tex = mesh.tex_id; }
                glBindVertexArray(mesh.vao);
                glDrawElementsInstanced(GL_TRIANGLES, mesh.n_indices, GL_UNSIGNED_INT, nullptr, ninst);
            }
        }
    };
    draw_pass(false);   // opaque first (depth write on)
    // Translucent pass: glow/decal/glass FX (smtranslucent — e.g. lamp-light
    // cones "s_dcl_lamplite1_a", billboard smoke). These must NOT write depth or
    // they occlude the city behind them as solid shapes (the "white cone spikes"
    // erupting from every streetlight). Keep depth TEST so they're hidden behind
    // solid geometry, but disable depth WRITE so they composite as transparent.
    glDepthMask(GL_FALSE);
    draw_pass(true);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);

    glUniform1i(uloc("uInstanced"), 0);   // restore for grid / other paths

    if (show_grid) {
        glUniformMatrix4fv(uloc("uMVP"), 1, GL_FALSE, glm::value_ptr(VP));
        glUniform1i(uloc("uWire"), 1);
        glUniform3f(uloc("uWireCol"), 0.22f, 0.22f, 0.26f);
        glBindVertexArray(m_grid_vao);
        glDrawArrays(GL_LINES, 0, m_grid_n);
        glBindVertexArray(0);
        glUniform1i(uloc("uWire"), 0);
    }

    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}
