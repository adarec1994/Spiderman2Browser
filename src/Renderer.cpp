#include "Renderer.h"
#include "Texture.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <iostream>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

// ── Shaders ───────────────────────────────────────────────
static const char* VERT_SRC = R"(
#version 330 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec2  aUV;
layout(location=2) in vec4  aBoneIdx;  // global bone indices as floats
layout(location=3) in vec4  aBoneWt;   // bone weights

uniform mat4  uMVP;
uniform float uPointSize;
uniform mat4  uBones[60];
uniform bool  uSkinned;

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
    gl_Position  = uMVP * pos;
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
uniform vec3 uWireCol;
out vec4 fragColor;
void main(){
    if(uWire){ fragColor=vec4(uWireCol,1.0); return; }
    if(uHasTex) fragColor=vec4(texture(uTex,vUV).rgb,1.0);
    else        fragColor=vec4(0.60,0.62,0.65,1.0);
}
)";

static unsigned int compile_shader(const char* src, GLenum type) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s,512,nullptr,log); std::cerr<<log<<"\n"; }
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

    glm::vec3 mn(1e9f), mx(-1e9f);
    for (auto& sm:model->submeshes) for (auto& p:sm.positions) { mn=glm::min(mn,p); mx=glm::max(mx,p); }
    gm->center = (mn+mx)*0.5f;
    gm->scale  = std::max(std::max(mx.x-mn.x, mx.y-mn.y), std::max(mx.z-mn.z, 1e-6f));

    for (auto& sm : model->submeshes) {
        GPUMesh m;
        m.mat_name  = sm.mat_name;
        m.n_indices = (int)sm.indices.size();
        m.tex_id    = find_texture(sm.tex_candidates.empty()
                          ? std::vector<std::string>{sm.tex_name}
                          : sm.tex_candidates, dir);
        std::cerr << "[MODEL] SM" << gm->meshes.size()
                  << " mat='" << sm.mat_name << "'"
                  << " tex=" << (m.tex_id ? "OK" : "MISSING")
                  << " candidates:";
        for (auto& c : sm.tex_candidates) std::cerr << " '" << c << "'";
        if (sm.tex_candidates.empty()) std::cerr << " '" << sm.tex_name << "'";
        std::cerr << "\n";

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

    glUseProgram(m_shader);
    glUniformMatrix4fv(uloc("uMVP"),1,GL_FALSE,glm::value_ptr(MVP));
    glUniform1i(uloc("uTex"),0);
    glUniform1i(uloc("uWire"),0);
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