#include "Renderer.h"
#include "Texture.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <filesystem>
#include <iostream>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

// ── Shaders ───────────────────────────────────────────────
static const char* VERT_SRC = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
uniform mat4  uMVP;
uniform float uPointSize;
out vec2 vUV;
void main(){
    gl_Position  = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
    vUV = aUV;
}
)";

static const char* FRAG_SRC = R"(
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform bool      uHasTex;
uniform bool      uWire;
uniform vec3      uWireCol;
out vec4 fragColor;
void main(){
    if(uWire){ fragColor = vec4(uWireCol, 1.0); return; }
    if(uHasTex)
        fragColor = vec4(texture(uTex, vUV).rgb, 1.0);
    else
        fragColor = vec4(0.60, 0.62, 0.65, 1.0);
}
)";

static unsigned int compile(const char* src, GLenum type) {
    unsigned int s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        std::cerr << "Shader error: " << log << "\n";
    }
    return s;
}

// ── GPUMesh ───────────────────────────────────────────────
void GPUMesh::draw() const {
    glBindVertexArray(vao);
    if (tex_id) { glBindTexture(GL_TEXTURE_2D, tex_id); glEnable(GL_TEXTURE_2D); }
    else          glDisable(GL_TEXTURE_2D);
    glDrawElements(GL_TRIANGLES, n_indices, GL_UNSIGNED_INT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}
void GPUMesh::release() {
    glDeleteVertexArrays(1,&vao); glDeleteBuffers(1,&vbo); glDeleteBuffers(1,&ibo);
    vao=vbo=ibo=0;
}

// ── GPUModel ──────────────────────────────────────────────
void GPUModel::draw() const { for (auto& m : meshes) m.draw(); }
void GPUModel::release()       { for (auto& m : meshes) m.release(); }

// ── GPUSkeleton ───────────────────────────────────────────
void GPUSkeleton::build(const Skeleton& sk) {
    // Lines: parent -> child
    std::vector<float> line_verts;
    for (auto& [a,b] : sk.lines) {
        auto& pa = sk.bones[a].world_pos;
        auto& pb = sk.bones[b].world_pos;
        line_verts.insert(line_verts.end(), {pa.x,pa.y,pa.z, pb.x,pb.y,pb.z});
    }
    n = (int)sk.lines.size() * 2;

    // Points: one per bone
    std::vector<float> pt_verts;
    for (auto& bone : sk.bones) {
        pt_verts.insert(pt_verts.end(),
            {bone.world_pos.x, bone.world_pos.y, bone.world_pos.z});
    }
    n_pts = (int)sk.bones.size();

    if (!vao) {
        glGenVertexArrays(1,&vao); glGenBuffers(1,&vbo);
        glGenVertexArrays(1,&pt_vao); glGenBuffers(1,&pt_vbo);
    }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, line_verts.size()*4, line_verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,12,nullptr);
    glBindVertexArray(0);

    glBindVertexArray(pt_vao);
    glBindBuffer(GL_ARRAY_BUFFER, pt_vbo);
    glBufferData(GL_ARRAY_BUFFER, pt_verts.size()*4, pt_verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,12,nullptr);
    glBindVertexArray(0);
}

void GPUSkeleton::draw() const {
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, n);
    glBindVertexArray(0);
}

void GPUSkeleton::draw_joints() const {
    glBindVertexArray(pt_vao);
    glDrawArrays(GL_POINTS, 0, n_pts);
    glBindVertexArray(0);
}

void GPUSkeleton::draw_joint(int i) const {
    if (i < 0 || i >= n_pts) return;
    glBindVertexArray(pt_vao);
    glDrawArrays(GL_POINTS, i, 1);
    glBindVertexArray(0);
}
void GPUSkeleton::release() {
    glDeleteVertexArrays(1,&vao);    glDeleteBuffers(1,&vbo);
    glDeleteVertexArrays(1,&pt_vao); glDeleteBuffers(1,&pt_vbo);
    vao=vbo=pt_vao=pt_vbo=0;
}

// ── Renderer ─────────────────────────────────────────────
void Renderer::init() {
    unsigned int vs = compile(VERT_SRC, GL_VERTEX_SHADER);
    unsigned int fs = compile(FRAG_SRC, GL_FRAGMENT_SHADER);
    m_shader = glCreateProgram();
    glAttachShader(m_shader, vs); glAttachShader(m_shader, fs);
    glLinkProgram(m_shader);
    glDeleteShader(vs); glDeleteShader(fs);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.10f, 0.10f, 0.13f, 1.f);

    build_grid(20, 0.1f);
}

void Renderer::shutdown() {
    glDeleteProgram(m_shader);
    glDeleteVertexArrays(1, &m_grid_vao);
    glDeleteBuffers(1, &m_grid_vbo);
}

int Renderer::uloc(const char* name) {
    return glGetUniformLocation(m_shader, name);
}

void Renderer::build_grid(int half, float step) {
    std::vector<float> lines;
    float t = half * step;
    for (int i = -half; i <= half; ++i) {
        float x = i * step;
        lines.insert(lines.end(), { x,-0.001f,-t,  x,-0.001f, t });
        lines.insert(lines.end(), {-t,-0.001f, x,  t,-0.001f, x });
    }
    m_grid_n = (int)lines.size() / 3;
    glGenVertexArrays(1,&m_grid_vao); glGenBuffers(1,&m_grid_vbo);
    glBindVertexArray(m_grid_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, lines.size()*4, lines.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,12,nullptr);
    glBindVertexArray(0);
}

GPUModel* Renderer::upload_model(const XBXModel* model) {
    if (!model) return nullptr;
    std::string model_dir = fs::path(model->filepath).parent_path().string();

    auto* gm = new GPUModel();

    // Compute AABB
    glm::vec3 mn( 1e9), mx(-1e9);
    for (auto& sm : model->submeshes)
        for (auto& p : sm.positions) {
            mn = glm::min(mn, p); mx = glm::max(mx, p);
        }
    gm->center = (mn + mx) * 0.5f;
    glm::vec3 ext = mx - mn;
    gm->scale  = std::max({ext.x, ext.y, ext.z, 1e-6f});

    for (auto& sm : model->submeshes) {
        GPUMesh m;
        m.mat_name  = sm.mat_name;
        m.n_indices = (int)sm.indices.size();
        m.tex_id    = find_texture(sm.tex_name, model_dir);

        // interleave XYZ + UV
        std::vector<float> vdata;
        vdata.reserve(sm.positions.size() * 5);
        for (size_t i = 0; i < sm.positions.size(); ++i) {
            vdata.push_back(sm.positions[i].x);
            vdata.push_back(sm.positions[i].y);
            vdata.push_back(sm.positions[i].z);
            vdata.push_back(sm.uvs[i].x);
            vdata.push_back(sm.uvs[i].y);
        }

        glGenVertexArrays(1,&m.vao); glGenBuffers(1,&m.vbo); glGenBuffers(1,&m.ibo);
        glBindVertexArray(m.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, vdata.size()*4, vdata.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sm.indices.size()*4, sm.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,20,(void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,20,(void*)12);
        glBindVertexArray(0);

        gm->meshes.push_back(std::move(m));
    }
    return gm;
}

GPUSkeleton* Renderer::upload_skeleton(const Skeleton* sk) {
    if (!sk) return nullptr;
    auto* gs = new GPUSkeleton();
    gs->build(*sk);
    return gs;
}

void Renderer::draw_scene(const Camera& cam, int vp_x, int vp_w, int vp_h,
                          const GPUModel* model, const GPUSkeleton* skel) {
    glViewport(vp_x, 0, vp_w, vp_h);
    glScissor(vp_x, 0, vp_w, vp_h);
    glEnable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!model) { glDisable(GL_SCISSOR_TEST); return; }

    float aspect = (float)vp_w / (float)std::max(vp_h, 1);

    // Model MVP with optional Y rotation
    glm::mat4 S  = glm::scale(glm::mat4(1), glm::vec3(1.f/model->scale));
    glm::mat4 T  = glm::translate(glm::mat4(1), -model->center);
    glm::mat4 Ry = glm::rotate(glm::mat4(1), model_rot_y, glm::vec3(0,1,0));
    glm::mat4 MVP = cam.proj(aspect) * cam.view() * S * Ry * T;

    glUseProgram(m_shader);
    glUniformMatrix4fv(uloc("uMVP"), 1, GL_FALSE, glm::value_ptr(MVP));
    glUniform1i(uloc("uTex"), 0);
    glUniform1i(uloc("uWire"), 0);
    glActiveTexture(GL_TEXTURE0);

    // Solid mesh
    for (auto& mesh : model->meshes) {
        glUniform1i(uloc("uHasTex"), mesh.tex_id ? 1 : 0);
        mesh.draw();
    }

    // Wireframe overlay
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glUniform1i(uloc("uWire"), 1);
        glUniform3f(uloc("uWireCol"), 0.2f, 0.9f, 0.4f);
        glLineWidth(1.2f);
        for (auto& mesh : model->meshes) mesh.draw();
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // Skeleton — always on top
    if (show_skel && skel && skel->n > 0) {
        // Skeleton is in the same raw coordinate space as the model,
        // so use same S and T but WITHOUT Ry (bones already in world pos)
        glm::mat4 skelMVP = cam.proj(aspect) * cam.view() * S * T;
        glUniformMatrix4fv(uloc("uMVP"), 1, GL_FALSE, glm::value_ptr(skelMVP));
        glUniform1i(uloc("uWire"), 1);

        glDisable(GL_DEPTH_TEST);

        // Bone lines
        glLineWidth(1.5f);
        glUniform3f(uloc("uWireCol"), 0.45f, 0.65f, 1.0f);
        skel->draw();
        glLineWidth(1.f);

        // All joint dots — white, 5px
        glEnable(GL_PROGRAM_POINT_SIZE);
        glUniform1f(uloc("uPointSize"), 5.f);
        glUniform3f(uloc("uWireCol"), 0.85f, 0.85f, 0.85f);
        skel->draw_joints();

        // Selected joint — bright orange, 9px
        if (sel_bone >= 0) {
            glUniform1f(uloc("uPointSize"), 9.f);
            glUniform3f(uloc("uWireCol"), 1.f, 0.55f, 0.05f);
            skel->draw_joint(sel_bone);
        }

        glDisable(GL_PROGRAM_POINT_SIZE);
        glEnable(GL_DEPTH_TEST);

        glUniformMatrix4fv(uloc("uMVP"), 1, GL_FALSE, glm::value_ptr(MVP));
    }

    // Grid
    if (show_grid) {
        glm::mat4 gridMVP = cam.proj(aspect) * cam.view();
        glUniformMatrix4fv(uloc("uMVP"), 1, GL_FALSE, glm::value_ptr(gridMVP));
        glUniform1i(uloc("uWire"), 1);
        glUniform3f(uloc("uWireCol"), 0.22f, 0.22f, 0.26f);
        glBindVertexArray(m_grid_vao);
        glDrawArrays(GL_LINES, 0, m_grid_n);
        glBindVertexArray(0);
    }

    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}