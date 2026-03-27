#include "App.h"
#include "Texture.h"
#include "WorldParser.h"
#include <glad/glad.h>
#include <fstream>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <map>
#include <cstring>
#include <cmath>

namespace fs = std::filesystem;
static App* g_app = nullptr;

static int vp_x()      { return UI::PANEL_W; }
static int vp_w(int W) { return std::max(W - UI::PANEL_W, 400); }

// ── Skinning ──────────────────────────────────────────────────────────────────

void App::compute_skinning() {
    for (int i = 0; i < N_BONES; ++i)
        m_skinning[i] = m_cur_pose[i] * m_inv_bind[i];
}

void App::upload_skinning() {
    compute_skinning();
    m_renderer.set_bone_matrices(m_skinning.data(), N_BONES);
}

// ── Animation ────────────────────────────────────────────────────────────────

void App::load_animations(const std::string& folder) {
    m_anim_clips = scan_animations(folder);
    m_anim_sel   = -1;
    m_anim_play  = false;
    m_anim_time  = 0.f;
    m_anim_bone_map.clear();

    m_ui_state.anim_names.clear();
    m_ui_state.anim_names.reserve(m_anim_clips.size());
    for (auto& c : m_anim_clips)
        m_ui_state.anim_names.push_back(c.name);

    std::cout << "Animations: " << m_anim_clips.size() << "\n";
}

void App::build_anim_bone_map(const AnimClip& clip) {
    int n_anim = (int)clip.track_count / 3;
    m_anim_bone_map.resize(n_anim, -1);

    // Black Cat (BC*) animations: tc=72 -> 24 animated bones.
    // Palette skips: fingers (12-26,33-47), foretwist (27-28,48-49),
    // headnub (6), head_bone (7). Derived from 60-bone skeleton dump.
    static const int BC_PALETTE[24] = {
         0,  // pelvis
         1,  // spine
         2,  // spine1
         3,  // spine2
         4,  // neck
         5,  // head
         8,  // l clavicle
         9,  // l upperarm
        10,  // l forearm
        11,  // l hand
        29,  // r clavicle
        30,  // r upperarm
        31,  // r forearm
        32,  // r hand
        50,  // l_breast
        51,  // r_breast
        52,  // l thigh
        53,  // l calf
        54,  // l foot
        55,  // l toe0
        56,  // r thigh
        57,  // r calf
        58,  // r foot
        59,  // r toe0
    };

    int n_skel = m_skeleton ? (int)m_skeleton->bones.size() : 0;
    for (int i = 0; i < n_anim; ++i) {
        if (n_anim == 24 && i < 24) {
            int si = BC_PALETTE[i];
            m_anim_bone_map[i] = (si < n_skel) ? si : -1;
        } else {
            // Fallback for other character types (CIV/COP): direct index
            m_anim_bone_map[i] = (i < n_skel) ? i : -1;
        }
    }
}

void App::select_animation(int idx) {
    if (idx < 0 || idx >= (int)m_anim_clips.size()) {
        m_anim_sel  = -1;
        m_anim_play = false;
        // Restore bind pose
        if (m_has_bones) {
            for (int i = 0; i < N_BONES; ++i) m_cur_pose[i] = m_bind_pose[i];
            upload_skinning();
        }
        return;
    }

    m_anim_sel  = idx;
    m_anim_time = 0.f;
    m_anim_play = false;

    AnimClip& clip = m_anim_clips[idx];
    if (!clip.loaded) parse_animation(clip);

    if (m_skeleton) build_anim_bone_map(clip);
    apply_animation_pose(0.f);
}

void App::apply_animation_pose(float t) {
    if (!m_has_bones) return;
    if (m_anim_sel < 0 || m_anim_sel >= (int)m_anim_clips.size()) return;

    const AnimClip& clip = m_anim_clips[m_anim_sel];
    if (!clip.loaded) return;

    // Start from bind pose
    for (int i = 0; i < N_BONES; ++i) m_cur_pose[i] = m_bind_pose[i];

    auto poses = clip.sample_pose(t);
    int n_anim = (int)poses.size();

    for (int ai = 0; ai < n_anim; ++ai) {
        int si = (ai < (int)m_anim_bone_map.size()) ? m_anim_bone_map[ai] : ai;
        if (si < 0 || si >= N_BONES) continue;

        const BonePose& bp = poses[ai];
        float angle = std::sqrt(bp.ax*bp.ax + bp.ay*bp.ay + bp.az*bp.az);
        if (angle < 1e-8f) continue;

        // Apply rotation in bone's local space (pre-multiply rotation into bind pose)
        glm::mat4 R = bp.to_matrix();
        m_cur_pose[si] = m_bind_pose[si] * R;
    }

    upload_skinning();
    if (m_skeleton) {
        for (int i = 0; i < N_BONES && i < (int)m_skeleton->bones.size(); ++i)
            m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);
        rebuild_skel_gpu();
    }
}

void App::tick_animation(double now) {
    if (!m_anim_play || m_anim_sel < 0) {
        m_last_frame = now;
        return;
    }

    double dt = now - m_last_frame;
    m_last_frame = now;
    if (dt > 0.1) dt = 0.1; // clamp large gaps

    const AnimClip& clip = m_anim_clips[m_anim_sel];
    float dur = clip.duration > 0 ? clip.duration
              : (clip.fps > 0 && clip.frame_count > 1
                 ? (float)(clip.frame_count - 1) / clip.fps : 1.f);

    m_anim_time += (float)dt;
    if (clip.loop) {
        if (dur > 0) m_anim_time = std::fmod(m_anim_time, dur);
    } else {
        if (m_anim_time >= dur) {
            m_anim_time = dur;
            m_anim_play = false;
            m_ui_state.anim_playing = false;
        }
    }
    m_ui_state.anim_time  = m_anim_time;
    m_ui_state.anim_dur   = dur;

    apply_animation_pose(m_anim_time);
}

// ── Picking ───────────────────────────────────────────────────────────────────

int App::pick_bone(double mx, double my) {
    if (!m_skeleton || !m_gpu_model) return -1;
    int w = vp_w(m_w), h = m_h;
    glm::vec3 ro = m_cam.eye();
    glm::vec3 rd = m_cam.ray_dir((float)mx,(float)my, vp_x(), w, h);
    float s = m_gpu_model->scale;
    glm::vec3 c = m_gpu_model->center;
    float pick_r = s * 0.03f;

    float best_t = 1e9f;
    int   best_i = -1;
    for (int i = 0; i < (int)m_skeleton->bones.size(); ++i) {
        glm::vec3 bp = (m_skeleton->bones[i].world_pos - c) / s;
        glm::vec3 oc = ro - bp;
        float b    = glm::dot(oc,rd);
        float r    = pick_r/s;
        float disc = b*b - glm::dot(oc,oc) + r*r;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t > 0 && t < best_t) { best_t=t; best_i=i; }
    }
    return best_i;
}

// Möller–Trumbore ray-triangle test
static bool ray_tri(glm::vec3 ro, glm::vec3 rd,
                    glm::vec3 v0, glm::vec3 v1, glm::vec3 v2, float& t) {
    const float EPS = 1e-7f;
    glm::vec3 e1=v1-v0, e2=v2-v0, h=glm::cross(rd,e2);
    float a = glm::dot(e1,h);
    if (fabsf(a)<EPS) return false;
    float f = 1.f/a;
    glm::vec3 s=ro-v0;
    float u = f*glm::dot(s,h);
    if (u<0.f||u>1.f) return false;
    glm::vec3 q=glm::cross(s,e1);
    float v = f*glm::dot(rd,q);
    if (v<0.f||u+v>1.f) return false;
    t = f*glm::dot(e2,q);
    return t>EPS;
}

// Build triangle index list from cached raw data + current method
static std::vector<uint32_t> build_tris_for_pick(
    const std::vector<uint16_t>& raw, uint32_t vc, int method_sel)
{
    std::vector<uint32_t> tris;
    if (!raw.empty()) {
        if (method_sel==1) { // TList
            for (size_t i=0;i+2<raw.size();i+=3) {
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2];
                if (a<vc&&b<vc&&c<vc&&a!=b&&b!=c&&a!=c){tris.push_back(a);tris.push_back(b);tris.push_back(c);}
            }
        } else if (method_sel==2) { // QuadList
            for (size_t i=0;i+3<raw.size();i+=4) {
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2],d=raw[i+3];
                if (a<vc&&b<vc&&c<vc&&d<vc){
                    tris.push_back(a);tris.push_back(b);tris.push_back(c);
                    tris.push_back(a);tris.push_back(c);tris.push_back(d);}
            }
        } else if (method_sel==3) { // TFan
            if (!raw.empty()&&raw[0]<vc)
                for (size_t i=1;i+1<raw.size();++i){
                    uint32_t b=raw[i],c=raw[i+1];
                    if (b<vc&&c<vc&&raw[0]!=b&&b!=c&&raw[0]!=c){tris.push_back(raw[0]);tris.push_back(b);tris.push_back(c);}
                }
        } else { // TStrip
            int p=0;
            for (size_t i=0;i+2<raw.size();++i){
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2];
                if (a>=vc||b>=vc||c>=vc){p^=1;continue;}
                if (a==b||b==c||a==c){p^=1;continue;}
                if (p){tris.push_back(a);tris.push_back(c);tris.push_back(b);}
                else  {tris.push_back(a);tris.push_back(b);tris.push_back(c);}
                p^=1;
            }
        }
    } else { // sequential strip
        int p=0;
        for (uint32_t i=0;i+2<vc;++i){
            if (p){tris.push_back(i);tris.push_back(i+2);tris.push_back(i+1);}
            else  {tris.push_back(i);tris.push_back(i+1);tris.push_back(i+2);}
            p^=1;
        }
    }
    return tris;
}

void App::try_select(double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;

    // ── Submesh pick ─────────────────────────────────────────────────────────
    if (m_gpu_model && !m_cached_raw.empty()) {
        int w = vp_w(m_w), h = m_h;
        glm::vec3 ro = m_cam.eye();
        glm::vec3 rd = m_cam.ray_dir((float)mx,(float)my, vp_x(), w, h);

        // Same model matrix as Renderer::draw_scene
        glm::mat4 T    = glm::translate(glm::mat4(1), -m_gpu_model->center);
        glm::mat4 Ry   = glm::rotate(glm::mat4(1), m_model_rot_y, glm::vec3(0,1,0));
        glm::mat4 S    = glm::scale(glm::mat4(1), glm::vec3(1.f/m_gpu_model->scale));
        glm::mat4 Minv = glm::inverse(S * Ry * T);

        // Ray in model-local space
        glm::vec3 lo = glm::vec3(Minv * glm::vec4(ro, 1.f));
        glm::vec3 ld = glm::normalize(glm::vec3(Minv * glm::vec4(rd, 0.f)));

        float best_t  = 1e30f;
        int   best_sm = -1;

        for (int si=0; si<(int)m_cached_raw.size(); ++si) {
            auto& rm = m_cached_raw[si];
            if (rm.positions.empty()) continue;
            int method = (si<(int)m_ui_state.submeshes.size()) ?
                          m_ui_state.submeshes[si].method_sel : 0;
            auto tris = build_tris_for_pick(rm.raw, rm.vc, method);
            for (size_t ti=0; ti+2<tris.size(); ti+=3) {
                float t;
                if (ray_tri(lo,ld, rm.positions[tris[ti]],
                                   rm.positions[tris[ti+1]],
                                   rm.positions[tris[ti+2]], t) && t<best_t) {
                    best_t=t; best_sm=si;
                }
            }
        }

        if (best_sm >= 0) {
            m_ui_state.sel_submesh = best_sm;
            m_renderer.sel_submesh = best_sm;
            // Update texture preview
            if (m_gpu_model && best_sm < (int)m_gpu_model->meshes.size()) {
                m_ui_state.preview_tex_id   = m_gpu_model->meshes[best_sm].tex_id;
                m_ui_state.preview_tex_name = best_sm < (int)m_ui_state.submeshes.size()
                    ? m_ui_state.submeshes[best_sm].tex_candidates.empty()
                        ? m_ui_state.submeshes[best_sm].mat_name
                        : m_ui_state.submeshes[best_sm].tex_candidates[0]
                    : "";
            }
            return;
        }
    }

    // ── Bone pick fallback ───────────────────────────────────────────────────
    int b = pick_bone(mx,my);
    m_sel_bone = b;
    m_renderer.sel_bone = b;
    if (b>=0 && m_skeleton)
        std::cout << "Bone " << b << ": " << m_skeleton->bones[b].name << "\n";
}

// ── Rotation ─────────────────────────────────────────────────────────────────

void App::begin_rotate(double mx) {
    if (!m_gpu_model) return;
    m_rot_mode    = true;
    m_rot_start_x = mx;
    m_rot_accum   = 0.f;
    m_rot_axis    = {0,1,0};
    if (m_sel_bone >= 0 && m_has_bones) m_cur_pose_backup = m_cur_pose;
}

void App::update_rotate(double mx) {
    if (!m_rot_mode) return;
    float angle = (float)(mx - m_rot_start_x) / (float)m_w * glm::two_pi<float>();
    float delta = angle - m_rot_accum;
    m_rot_accum  = angle;
    glm::mat4 R  = glm::rotate(glm::mat4(1), delta, m_rot_axis);

    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        glm::vec3 pivot = glm::vec3(m_cur_pose[m_sel_bone][3]);
        int nb = (int)m_skeleton->bones.size();
        std::vector<bool> aff(nb, false);
        aff[m_sel_bone] = true;
        for (int i = 0; i < nb; ++i)
            if (m_skeleton->bones[i].parent >= 0 && aff[m_skeleton->bones[i].parent])
                aff[i] = true;

        for (int i = 0; i < nb; ++i) {
            if (!aff[i]) continue;
            glm::mat4& M = m_cur_pose[i];
            glm::vec3 t  = glm::vec3(M[3]);
            M    = R * M;
            M[3] = glm::vec4(glm::vec3(R * glm::vec4(t - pivot, 0.f)) + pivot, 1.f);
        }
        for (int i = 0; i < nb; ++i)
            if (aff[i]) m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);

        upload_skinning();
        rebuild_skel_gpu();
    } else {
        m_model_rot_y += delta;
        m_renderer.model_rot_y = m_model_rot_y;
    }
}

void App::confirm_rotate() { m_rot_mode = false; }

void App::cancel_rotate() {
    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        m_cur_pose = m_cur_pose_backup;
        for (int i = 0; i < (int)m_skeleton->bones.size(); ++i)
            m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);
        upload_skinning();
        rebuild_skel_gpu();
    } else {
        m_model_rot_y -= m_rot_accum;
        m_renderer.model_rot_y = m_model_rot_y;
    }
    m_rot_mode = false;
}

void App::rebuild_skel_gpu() {
    if (m_gpu_skel && m_skeleton) m_gpu_skel->build(*m_skeleton);
}

// ── GLFW callbacks ────────────────────────────────────────────────────────────

void App::cb_key(GLFWwindow* w, int key, int, int action, int) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) {
            if (g_app->m_rot_mode) g_app->cancel_rotate();
            else glfwSetWindowShouldClose(w,1);
        }
        if (key == GLFW_KEY_R && !g_app->m_rot_mode) {
            double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
            g_app->begin_rotate(mx);
        }
        if (g_app->m_rot_mode) {
            if (key==GLFW_KEY_X) g_app->m_rot_axis={1,0,0};
            if (key==GLFW_KEY_Y) g_app->m_rot_axis={0,1,0};
            if (key==GLFW_KEY_Z) g_app->m_rot_axis={0,0,1};
        }
        // Space = play/stop animation
        if (key == GLFW_KEY_SPACE) {
            if (g_app->m_anim_sel >= 0) {
                g_app->m_anim_play          = !g_app->m_anim_play;
                g_app->m_ui_state.anim_playing = g_app->m_anim_play;
                g_app->m_last_frame         = glfwGetTime();
                if (!g_app->m_anim_play) {
                    // Stop: snap to nearest frame
                    g_app->apply_animation_pose(g_app->m_anim_time);
                }
            }
        }
    }
    if ((action==GLFW_PRESS||action==GLFW_REPEAT) && key==GLFW_KEY_ENTER && g_app->m_rot_mode)
        g_app->confirm_rotate();
}

void App::cb_mouse_btn(GLFWwindow*, int btn, int action, int) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
    bool in_vp = mx >= vp_x();

    if (action == GLFW_PRESS) {
        if (btn == GLFW_MOUSE_BUTTON_LEFT) {
            if (g_app->m_rot_mode) g_app->confirm_rotate();
            else if (in_vp) {
                if (!g_app->m_world_mode) g_app->try_select(mx,my);
                g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
            }
        }
        if (btn == GLFW_MOUSE_BUTTON_RIGHT && in_vp) {
            if (g_app->m_rot_mode) { g_app->cancel_rotate(); }
            else if (g_app->m_world_mode && g_app->m_cam.fly) {
                // RMB in world/fly mode = mouse-look; capture cursor
                g_app->m_fly_look = true;
                g_app->m_lx = mx; g_app->m_ly = my;
                glfwSetInputMode(g_app->m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            } else {
                g_app->m_drag_r=true; g_app->m_lx=mx; g_app->m_ly=my;
            }
        }
        if (btn == GLFW_MOUSE_BUTTON_MIDDLE && in_vp) {
            g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
        }
    } else {
        if (btn==GLFW_MOUSE_BUTTON_LEFT||btn==GLFW_MOUSE_BUTTON_MIDDLE) g_app->m_drag_l=false;
        if (btn==GLFW_MOUSE_BUTTON_RIGHT) {
            g_app->m_drag_r=false;
            if (g_app->m_fly_look) {
                g_app->m_fly_look = false;
                glfwSetInputMode(g_app->m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }
}

void App::cb_cursor(GLFWwindow*, double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double dx=mx-g_app->m_lx, dy=my-g_app->m_ly;
    g_app->m_lx=mx; g_app->m_ly=my;
    if (g_app->m_rot_mode) { g_app->update_rotate(mx); return; }
    if (g_app->m_fly_look) { g_app->m_cam.fly_look((float)dx,(float)dy); return; }
    if (g_app->m_drag_l)   g_app->m_cam.orbit((float)dx,(float)dy);
    if (g_app->m_drag_r)   g_app->m_cam.do_pan((float)dx,(float)dy);
}

void App::cb_scroll(GLFWwindow*, double, double dy) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
    if (mx>=vp_x()) g_app->m_cam.zoom((float)dy);
}

void App::cb_resize(GLFWwindow*, int w, int h) {
    g_app->m_w=w; g_app->m_h=h; glViewport(0,0,w,h);
}

// ── Init ──────────────────────────────────────────────────────────────────────

bool App::init(int w, int h, const char* title) {
    g_app=this; m_w=w; m_h=h;
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#endif
    m_window = glfwCreateWindow(w,h,title,nullptr,nullptr);
    if (!m_window) { glfwTerminate(); return false; }
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return false;

    glfwSetKeyCallback            (m_window,cb_key);
    glfwSetMouseButtonCallback    (m_window,cb_mouse_btn);
    glfwSetCursorPosCallback      (m_window,cb_cursor);
    glfwSetScrollCallback         (m_window,cb_scroll);
    glfwSetFramebufferSizeCallback(m_window,cb_resize);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding=4.f;
    ImGui_ImplGlfw_InitForOpenGL(m_window,true);
    ImGui_ImplOpenGL3_Init("#version 330");

    m_renderer.init();
    setup_callbacks();

    std::string saved = UI::load_folder();
    if (!saved.empty() && fs::exists(saved)) {
        m_ui_state.folder = saved;
        scan_folder(saved);
    }
    return true;
}

void App::setup_callbacks() {
    m_ui_cb.on_scan_folder  = [this](const std::string& f){ scan_folder(f); };
    m_ui_cb.on_select_file  = [this](int i){ m_ui_state.selected=i; load_file(i); };
    m_ui_cb.on_reset_camera = [this](){ m_cam.reset(); m_model_rot_y=0.f; m_renderer.model_rot_y=0.f; };
    m_ui_cb.on_select_anim  = [this](int i){ select_animation(i); m_ui_state.anim_sel=i; };
    m_ui_cb.on_prim_override = [this](int smi, int sel) {
        if (!m_gpu_model) return;
        if (smi >= 0 && smi < (int)m_ui_state.submeshes.size())
            m_ui_state.submeshes[smi].prim_method = sel==0?"TStrip":sel==1?"TList":sel==2?"QuadList":"TFan";
        rebuild_prim_override(smi, sel);
    };
    m_ui_cb.on_tex_assign = [this](int smi, const std::string& stem) {
        if (!m_gpu_model || smi < 0 || smi >= (int)m_gpu_model->meshes.size()) return;
        std::string dir = fs::path(m_ui_state.files[m_ui_state.selected]).parent_path().string();
        unsigned int tid = find_texture(stem, dir);
        m_gpu_model->meshes[smi].tex_id = tid;
        if (smi < (int)m_ui_state.submeshes.size())
            m_ui_state.submeshes[smi].has_tex = tid != 0;
        std::cerr << "[MATASSIGN] SM" << smi << " -> '" << stem << "' " << (tid?"OK":"MISS") << "\n";
    };
    m_ui_cb.on_tex_override = [this](int smi, int sel) {
        if (!m_gpu_model) return;
        if (smi < 0 || smi >= (int)m_ui_state.submeshes.size()) return;
        auto& si = m_ui_state.submeshes[smi];
        if (sel < 0 || sel >= (int)si.tex_candidates.size()) return;
        std::string dir = fs::path(m_ui_state.files[m_ui_state.selected]).parent_path().string();
        unsigned int tid = find_texture(si.tex_candidates[sel], dir);
        m_gpu_model->meshes[smi].tex_id = tid;
        si.has_tex = tid != 0;
        std::cerr << "[TEXOV] SM" << smi << " -> '" << si.tex_candidates[sel]
                  << "' " << (tid ? "OK" : "MISS") << "\n";
    };
    m_ui_cb.on_play_anim    = [this](){
        if (m_anim_sel<0) return;
        m_anim_play = !m_anim_play;
        m_ui_state.anim_playing = m_anim_play;
        m_last_frame = glfwGetTime();
    };
    m_ui_cb.on_load_world_file = [this](int i){
        if (i >= 0 && i < (int)m_ui_state.world_files.size())
            load_world(m_ui_state.world_files[i]);
    };
    m_ui_cb.on_load_all_worlds = [this](){ load_all_worlds(); };
}

// ── Scan + Load ───────────────────────────────────────────────────────────────

void App::scan_folder(const std::string& folder) {
    m_ui_state.files.clear();
    m_ui_state.selected=-1;
    m_ui_state.folder=folder;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(folder,ec)) {
        if (ec) break;
        auto p   = e.path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        if (ext==".xbx") m_ui_state.files.push_back(p.string());
    }
    std::sort(m_ui_state.files.begin(),m_ui_state.files.end());
    m_ui_state.status_msg = std::to_string(m_ui_state.files.size())+" files found";

    // Build global texture registry for cross-pack resolution
    build_tex_registry(folder);
    get_registry_entries(m_ui_state.all_tex_entries);

    // ── Mat string frequency report ───────────────────────────────────────────
    {
        struct SMInfo {
            std::string file;
            int         sm;
            std::string mat_name;   // +0x04
            std::string shader;     // +0x24
            std::string tex_name;   // +0x44
        };
        // tex_name → list of occurrences (only tex_name needs deduplication count)
        std::map<std::string, std::vector<SMInfo>> by_tex;

        auto rd32l = [](const uint8_t* d, size_t o) -> uint32_t {
            uint32_t v; memcpy(&v, d+o, 4); return v;
        };
        auto rdstr = [](const uint8_t* d, size_t o, size_t sz, int n=28) -> std::string {
            if (o+1 > sz) return {};
            size_t avail = std::min((size_t)n, sz-o);
            const char* s = reinterpret_cast<const char*>(d+o);
            size_t len = strnlen(s, avail);
            // reject binary garbage
            int printable = 0;
            for (size_t i=0;i<len;++i) if ((unsigned char)s[i]>=32&&(unsigned char)s[i]<127) ++printable;
            if (len > 0 && printable < (int)len/2) return {};
            return std::string(s, len);
        };

        for (auto& path : m_ui_state.files) {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) continue;
            size_t sz = f.tellg(); f.seekg(0);
            if (sz < 16) continue;
            std::vector<uint8_t> buf(sz);
            f.read(reinterpret_cast<char*>(buf.data()), sz);
            const uint8_t* d = buf.data();
            if (memcmp(d, "XBXM", 4) != 0) continue;

            uint32_t chunk_cnt = rd32l(d, 0x08);
            uint32_t hdr_sz    = rd32l(d, 0x0c);
            uint32_t geom_base = 0;
            for (uint32_t ci = 0; ci < chunk_cnt && !geom_base; ++ci) {
                uint32_t base = hdr_sz + ci*0x30;
                if (base+48 > sz) break;
                for (uint32_t i = 0; i < 12; ++i) {
                    uint32_t v = rd32l(d, base+i*4);
                    if ((v & 0xFF000000) == 0x02000000 && i+1 < 12) {
                        geom_base = rd32l(d, base+(i+1)*4)+4; break;
                    }
                }
            }
            if (!geom_base || geom_base+0x48 > sz) continue;
            uint32_t sm_cnt = rd32l(d, geom_base+0x04);
            if (sm_cnt == 0 || sm_cnt > 64) continue;

            std::string shortpath = (fs::path(path).parent_path().filename()
                                   / fs::path(path).filename()).string();

            for (uint32_t si = 0; si < sm_cnt; ++si) {
                uint32_t ptr = rd32l(d, geom_base+0x40+si*8);
                if (ptr+0x60 > sz) continue;
                uint32_t mat_ptr = rd32l(d, ptr);
                if (mat_ptr+0x50 > sz) continue;

                std::string mat  = rdstr(d, mat_ptr+0x04, sz);
                std::string shdr = rdstr(d, mat_ptr+0x24, sz);
                std::string tex  = rdstr(d, mat_ptr+0x44, sz);

                // key by tex_name (empty tex goes under "")
                by_tex[tex].push_back({shortpath, (int)si, mat, shdr, tex});
            }
        }

        // key by shader (+0x24), sorted by count desc
        std::map<std::string, std::vector<SMInfo>> by_shader;
        for (auto& [k,v] : by_tex)
            for (auto& loc : v)
                by_shader[loc.shader].push_back(loc);

        std::vector<std::pair<std::string,std::vector<SMInfo>>> multi;
        for (auto& [k,v] : by_shader) if (v.size() > 1) multi.push_back({k,v});
        std::sort(multi.begin(), multi.end(),
            [](auto& a, auto& b){ return a.second.size() > b.second.size(); });

        std::ofstream out("mat_strings_report.txt");
        if (out) {
            out << "XBX Mat/Shader/Texture Report\n";
            out << "Root: " << folder << "\n";
            out << "Files: " << m_ui_state.files.size() << "\n";
            out << "Shader types appearing >1 time: " << multi.size() << "\n";
            out << std::string(72,'=') << "\n\n";
            for (auto& [shader, locs] : multi) {
                out << "SHADER: \"" << shader << "\"  x" << locs.size() << "\n";
                for (auto& loc : locs)
                    out << "  SM" << loc.sm
                        << "  mat=\""  << loc.mat_name << "\""
                        << "  tex=\""  << loc.tex_name << "\""
                        << "  " << loc.file << "\n";
                out << "\n";
            }
            std::cout << "[SCAN] mat_strings_report.txt written (" << multi.size() << " shader types)\n";
        }
    }
    // ─────────────────────────────────────────────────────────────────────────

    // Scan for animations in same folder
    load_animations(folder);

    // ── World .dat files: bare stem (no _NNNNNNNN suffix) ────────────────────
    m_ui_state.world_files.clear();
    {
        std::error_code ecw;
        for (auto& entry : fs::recursive_directory_iterator(folder, ecw)) {
            if (ecw) { ecw.clear(); continue; }
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".dat") continue;
            std::string stem = p.stem().string();
            // World area files have cell-ID stems: 1-3 uppercase letters + 2-3 digits
            // + optional trailing letter (e.g. A01, F48, G60, A01R, C22I01).
            // Everything else (ARMORED_THUG, BLACK_CAT, BCRUN, etc.) is excluded.
            {
                // Must have no underscore and match [A-Z]{1,3}[0-9]{2,3}[A-Z]?
                if (stem.find('_') != std::string::npos) continue;
                size_t i = 0;
                // 1-3 leading letters
                while (i < stem.size() && std::isupper((unsigned char)stem[i])) ++i;
                if (i < 1 || i > 3) continue;
                size_t alpha_end = i;
                // 2-3 digits
                while (i < stem.size() && std::isdigit((unsigned char)stem[i])) ++i;
                if (i - alpha_end < 2 || i - alpha_end > 3) continue;
                // optional trailing letter(s)
                while (i < stem.size() && std::isupper((unsigned char)stem[i])) ++i;
                if (i != stem.size()) continue; // unexpected chars
            }
            m_ui_state.world_files.push_back(p.string());
        }
        std::sort(m_ui_state.world_files.begin(), m_ui_state.world_files.end());
        std::cout << "[WORLD_SCAN] " << m_ui_state.world_files.size() << " world dat files\n";
    }

    // ── XBX registry: lowercase_stem → absolute_path ─────────────────────────
    m_xbx_registry.clear();
    {
        std::error_code ecx;
        for (auto& entry : fs::recursive_directory_iterator(folder, ecx)) {
            if (ecx) { ecx.clear(); continue; }
            if (!entry.is_regular_file()) continue;
            std::string fnl = entry.path().filename().string();
            std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
            if (fs::path(fnl).extension() != ".xbx") continue;
            std::string stem = fs::path(fnl).stem().string();
            if (m_xbx_registry.find(stem) == m_xbx_registry.end())
                m_xbx_registry[stem] = entry.path().string();
        }
        std::cout << "[XBX_REG] " << m_xbx_registry.size() << " XBX files indexed\n";

        // Build base index: strip trailing digits/underscore-digits from each stem
        // so lookups like "s_trfflitea" instantly find "s_trfflitea_00000001"
        m_xbx_base_index.clear();
        for (auto& [stem, path] : m_xbx_registry) {
            // Pattern A: base_00000001 -> base
            std::string base = stem;
            if (base.size() > 9) {
                std::string tail = base.substr(base.size() - 9);
                if (tail[0] == '_' && std::all_of(tail.begin()+1, tail.end(), ::isdigit))
                    base = base.substr(0, base.size() - 9);
            }
            // Pattern B: base000 -> base (trailing digits only)
            if (base == stem) {
                while (!base.empty() && std::isdigit((unsigned char)base.back()))
                    base.pop_back();
            }
            if (base != stem && base.size() >= 3) {
                // keep the lexicographically smallest stem for each base
                auto it = m_xbx_base_index.find(base);
                if (it == m_xbx_base_index.end() || stem < it->second)
                    m_xbx_base_index[base] = stem;
            }
        }
        std::cout << "[XBX_REG] base index: " << m_xbx_base_index.size() << " entries\n";

        // Build suffix index: for each stem, index every underscore-split suffix
        // so "s_strtlampb_00000001" is findable via "strtlampb"
        m_xbx_suffix_index.clear();
        for (auto& [stem, path] : m_xbx_registry) {
            std::string s = stem;
            while (!s.empty()) {
                // strip trailing digits and underscores to get base suffix
                std::string base = s;
                while (!base.empty() && (std::isdigit((unsigned char)base.back()) || base.back() == '_'))
                    base.pop_back();
                if (base.size() >= 4 && m_xbx_suffix_index.find(base) == m_xbx_suffix_index.end())
                    m_xbx_suffix_index[base] = stem;
                auto us = s.find('_');
                if (us == std::string::npos) break;
                s = s.substr(us + 1);
            }
        }
        std::cout << "[XBX_REG] suffix index: " << m_xbx_suffix_index.size() << " entries\n";

        // Dump all XBX stems to a text file alongside the exe
        {
            std::ofstream out("xbx_mesh_list.txt");
            if (out) {
                std::vector<std::string> stems;
                stems.reserve(m_xbx_registry.size());
                for (auto& [stem, path] : m_xbx_registry)
                    stems.push_back(stem);
                std::sort(stems.begin(), stems.end());
                for (auto& s : stems)
                    out << s << "\n";
                std::cout << "[XBX_REG] wrote xbx_mesh_list.txt ("
                          << stems.size() << " entries)\n";
            }
        }
    }
}

// ── World loading ─────────────────────────────────────────────────────────────

void App::clear_world() {
    for (auto& [k, gm] : m_world_gpu_cache) { if (gm) { gm->release(); delete gm; } }
    m_world_gpu_cache.clear();
    m_world_draws.clear();
    m_world_mode = false;
    m_cam.fly    = false;    // restore orbit camera
    m_fly_look   = false;
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    m_ui_state.world_mode            = false;
    m_ui_state.world_instance_count  = 0;
    m_ui_state.world_prop_count      = 0;
    m_ui_state.world_dat_path        = {};
    m_ui_state.world_load_progress   = -1.f;
    m_ui_state.world_load_status     = {};
}

GPUModel* App::world_get_or_load_model(const std::string& asset_name) {
    if (asset_name.empty()) return nullptr;

    // Garbage filter: real names are ALL-CAPS or all-lowercase. Binary junk is mixed (e.g. "h4bC").
    {
        bool has_upper = false, has_lower = false;
        for (char ch : asset_name) {
            if (std::isupper((unsigned char)ch)) has_upper = true;
            if (std::islower((unsigned char)ch)) has_lower = true;
        }
        if (has_upper && has_lower) return nullptr;
    }

    std::string key = asset_name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = m_world_gpu_cache.find(key);
    if (it != m_world_gpu_cache.end()) return it->second;

    auto try_load = [&](const std::string& stem) -> GPUModel* {
        auto rit = m_xbx_registry.find(stem);
        if (rit == m_xbx_registry.end()) return nullptr;
        XBXModel* xm = parse_xbx(rit->second);
        if (!xm) {
            std::cout << "[PARSE_FAIL] " << stem << " @ " << rit->second << "\n";
            return nullptr;
        }
        GPUModel* gm = m_renderer.upload_model(xm);
        delete xm;
        return gm;
    };

    auto try_stem = [&](const std::string& stem) -> GPUModel* {
        if (stem.size() < 3) return nullptr;

        // 1. exact
        if (auto* gm = try_load(stem)) return gm;

        // strip trailing digits and underscores to get base
        std::string base = stem;
        while (!base.empty() && (std::isdigit((unsigned char)base.back()) || base.back() == '_'))
            base.pop_back();
        if (base.empty() || base.size() < 3) return nullptr;

        // 2. base exact  (sg_stor_15d000 -> sg_stor_15d)
        if (base != stem)
            if (auto* gm = try_load(base)) return gm;

        // 3. base index O(1)
        {
            auto bi = m_xbx_base_index.find(base);
            if (bi != m_xbx_base_index.end())
                if (auto* gm = try_load(bi->second)) return gm;
        }

        // 4. suffix index - only for long-enough bases to avoid false matches
        //    (min 8 chars: strtlampb=9, rfaccessa=9, indroofc=8, trfflitea=9)
        //    (blocked: helipad=7, tanka=5 etc.)
        if (base.size() >= 8) {
            // exact suffix index hit
            auto si = m_xbx_suffix_index.find(base);
            if (si != m_xbx_suffix_index.end())
                if (auto* gm = try_load(si->second)) return gm;

            // prefix scan within suffix index (trfflite -> trfflitea, trffliteb...)
            // suffix_index is small (~5k entries) so this is fast
            for (auto& [k, v] : m_xbx_suffix_index) {
                if (k.size() > base.size() && k.rfind(base, 0) == 0)
                    if (auto* gm = try_load(v)) return gm;
            }
        }

        return nullptr;
    };

    // Walk underscore-split suffixes longest first, max 4 strips
    std::string cur = key;
    int strips = 0;
    while (!cur.empty() && strips <= 4) {
        if (auto* gm = try_stem(cur)) { m_world_gpu_cache[key] = gm; return gm; }
        auto us = cur.find('_');
        if (us == std::string::npos) break;
        cur = cur.substr(us + 1);
        ++strips;
    }

    m_world_gpu_cache[key] = nullptr;
    std::cout << "[WORLD_MISS] " << asset_name << "\n";
    return nullptr;
}








// Build draw calls from one parsed WorldData, appending into m_world_draws.
// Helper: given a GPUModel, return the matrix that un-normalises its vertices
// back to their original coordinate space. draw_scene applies S*T internally;
// for world rendering we pre-bake it so the renderer just does MVP = VP * xform.
// upload_model normalises each model's vertices: v_stored = (v_local - center) / scale
// draw_scene undoes this with M = scale(1/scale) * translate(-center) for single-model view.
// For world rendering we need the inverse un-normalisation THEN the world placement:
//   final = world_xform * translate(center) * scale(scale_factor)
// This maps stored vertices back to local space, then into the world.
void App::build_world_draws(const WorldData& wd) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s;
    };

    for (auto& inst : wd.instances) {
        GPUModel* gm = world_get_or_load_model(inst.asset_name);
        if (!gm) continue;
        m_world_draws.push_back({ gm, inst.transform });
    }
    for (auto& prop : wd.props) {
        if (prop.type_idx < 0 || prop.type_idx >= (int)wd.prop_types.size()) continue;
        GPUModel* gm = world_get_or_load_model(wd.prop_types[prop.type_idx]);
        if (!gm) continue;
        float yr = glm::radians(prop.yaw_deg);
        glm::mat4 xf = glm::translate(glm::mat4(1.f), glm::vec3(prop.x, prop.y, prop.z));
        xf = glm::rotate(xf, yr, glm::vec3(0.f, 1.f, 0.f));
        m_world_draws.push_back({ gm, xf });
    }

    // Debug: show what each name resolved to
    std::cout << "[BUILD] prop_types->resolved:\n";
    for (auto& pt : wd.prop_types) {
        auto it = m_world_gpu_cache.find(lower(pt));
        bool hit = (it != m_world_gpu_cache.end() && it->second);
        std::cout << "  " << pt << " -> " << (hit ? lower(pt) : "MISS") << "\n";
    }
    std::cout << "[BUILD] instances->resolved:\n";
    for (auto& inst : wd.instances) {
        auto it = m_world_gpu_cache.find(lower(inst.asset_name));
        bool hit = (it != m_world_gpu_cache.end() && it->second);
        std::cout << "  " << inst.asset_name << " -> " << (hit ? lower(inst.asset_name) : "MISS") << "\n";
    }
}

void App::recentre_camera_on_world() {
    if (m_world_draws.empty()) return;

    // Compute bounding box of all draw call origins
    glm::vec3 mn(1e9f), mx(-1e9f);
    for (auto& dc : m_world_draws) {
        glm::vec3 p = glm::vec3(dc.xform[3]);
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    glm::vec3 centre = (mn + mx) * 0.5f;
    float     extent = glm::length(mx - mn);

    // Start above and behind the scene, looking toward centre
    glm::vec3 start = centre + glm::vec3(0.f, extent * 0.15f, extent * 0.4f);
    m_cam.reset_fly(start);
    m_cam.fly_speed = glm::clamp(extent * 0.05f, 10.f, 2000.f);

    // Aim the camera at the scene centre
    glm::vec3 dir = glm::normalize(centre - start);
    m_cam.yaw   = glm::degrees(atan2f(dir.x, dir.z));
    m_cam.pitch = glm::degrees(asinf(glm::clamp(dir.y, -1.f, 1.f)));

    std::cout << "[CAM] world centre=(" << centre.x << "," << centre.y << "," << centre.z
              << ") extent=" << extent << " start=(" << start.x << "," << start.y << "," << start.z << ")\n";
}

// Load the terrain XBX for a sector: same stem as the .dat, same directory.
// e.g. CITY_STRIP_A/A01/A01.dat  ->  looks for A01 in xbx_registry
// Adds it at identity so it renders at world origin (terrain is pre-placed in world space).
void App::load_sector_terrain(const std::string& dat_path) {
    std::string stem = fs::path(dat_path).stem().string();
    std::string key  = stem;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    // Try stem then stem+"r" (terrain XBX is always named {sector}r)
    GPUModel* gm = world_get_or_load_model(key);
    if (!gm) gm = world_get_or_load_model(key + "r");
    if (gm) {
        m_world_draws.push_back({ gm, glm::mat4(1.f) });
        std::cout << "[TERRAIN] loaded " << stem << "\n";
    } else {
        std::cout << "[TERRAIN_MISS] " << stem << "\n";
    }
}

void App::load_world(const std::string& dat_path) {
    clear_world();
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; m_gpu_model = nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel;  m_gpu_skel  = nullptr; }
    m_ui_state.has_model = false;
    m_ui_state.submeshes.clear();

    m_ui_state.world_load_progress = 0.f;
    m_ui_state.world_load_status   = "Parsing...";

    WorldData* wd = parse_world(dat_path);
    if (!wd) {
        m_ui_state.world_load_progress = -1.f;
        m_ui_state.status_msg = "Error: failed to parse world file";
        return;
    }

    int total = (int)wd->instances.size() + (int)wd->props.size();
    int done  = 0;

    m_ui_state.world_load_status = "Loading models...";
    for (auto& inst : wd->instances) {
        world_get_or_load_model(inst.asset_name);
        m_ui_state.world_load_progress = total > 0 ? (float)++done / total : 1.f;
    }
    for (auto& prop : wd->props) {
        if (prop.type_idx >= 0 && prop.type_idx < (int)wd->prop_types.size())
            world_get_or_load_model(wd->prop_types[prop.type_idx]);
        m_ui_state.world_load_progress = total > 0 ? (float)++done / total : 1.f;
    }

    build_world_draws(*wd);
    load_sector_terrain(dat_path);
    recentre_camera_on_world();
    m_ui_state.world_instance_count  = (int)wd->instances.size();
    m_ui_state.world_prop_count      = (int)wd->props.size();
    m_ui_state.world_dat_path        = dat_path;
    m_ui_state.world_load_progress   = -1.f;  // hide bar
    m_ui_state.status_msg            = "World loaded";
    std::cout << "[WORLD] " << m_world_draws.size() << " draw calls\n";
    delete wd;
}

void App::load_all_worlds() {
    clear_world();
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; m_gpu_model = nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel;  m_gpu_skel  = nullptr; }
    m_ui_state.has_model = false;
    m_ui_state.submeshes.clear();

    int n_files = (int)m_ui_state.world_files.size();
    if (n_files == 0) return;

    int total_inst = 0, total_props = 0;
    for (int fi = 0; fi < n_files; ++fi) {
        m_ui_state.world_load_progress = (float)fi / n_files;
        m_ui_state.world_load_status   = fs::path(m_ui_state.world_files[fi]).filename().string();

        WorldData* wd = parse_world(m_ui_state.world_files[fi]);
        if (!wd) continue;
        total_inst  += (int)wd->instances.size();
        total_props += (int)wd->props.size();
        build_world_draws(*wd);
        load_sector_terrain(m_ui_state.world_files[fi]);
        delete wd;
    }

    recentre_camera_on_world();
    m_world_mode                     = true;
    m_ui_state.world_mode            = true;
    m_ui_state.world_instance_count  = total_inst;
    m_ui_state.world_prop_count      = total_props;
    m_ui_state.world_dat_path        = "(all)";
    m_ui_state.world_load_progress   = -1.f;
    m_ui_state.status_msg            = "All worlds loaded";
    std::cout << "[WORLD] all — " << m_world_draws.size() << " draw calls\n";
}

void App::load_file(int idx) {
    if (idx<0||idx>=(int)m_ui_state.files.size()) return;
    const std::string& path = m_ui_state.files[idx];

    if (m_world_mode) clear_world();
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; m_gpu_model=nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel;  m_gpu_skel=nullptr; }
    m_skeleton.reset();
    m_has_bones=false;
    m_ui_state.has_model=false;
    m_ui_state.submeshes.clear();
    m_ui_state.status_msg="Loading...";
    m_sel_bone=-1; m_renderer.sel_bone=-1;
    m_model_rot_y=0.f; m_renderer.model_rot_y=0.f;
    m_anim_sel=-1; m_anim_play=false; m_anim_time=0.f;
    m_ui_state.anim_sel=-1; m_ui_state.anim_playing=false;

    XBXModel* model = parse_xbx(path);
    if (!model) { m_ui_state.status_msg="Error: Not XBXM or no geometry"; return; }

    m_gpu_model = m_renderer.upload_model(model);
    m_ui_state.has_model=true;
    m_ui_state.status_msg="Loaded";

    // Load animations from the same directory as the model file
    load_animations(fs::path(path).parent_path().string());

    m_cached_raw.clear();
    m_cached_raw.resize(model->submeshes.size());
    m_ui_state.submeshes.clear();
    m_ui_state.sel_submesh = -1;
    m_renderer.sel_submesh = -1;
    m_ui_state.preview_tex_id = 0;
    m_ui_state.preview_tex_name.clear();
    for (int i=0;i<(int)model->submeshes.size();++i) {
        auto& sm = model->submeshes[i];
        m_cached_raw[i].raw       = sm.raw_indices;
        m_cached_raw[i].vc        = (uint32_t)sm.positions.size();
        m_cached_raw[i].positions = sm.positions;

        // Determine default method_sel from prim_type
        int default_sel = 0; // TStrip
        if (sm.prim_type == 5) default_sel = 1; // TList
        if (sm.prim_type == 8) default_sel = 2; // QuadList

        const char* method_label = "TStrip";
        if (default_sel == 1) method_label = "TList";
        if (default_sel == 2) method_label = "QuadList";

        SubmeshInfo si;
        si.mat_name      = sm.mat_name;
        si.shader_type   = sm.shader_type;
        si.tex_candidates = sm.tex_candidates;
        si.tex_sel       = 0;
        si.tri_count     = (int)sm.indices.size()/3;
        si.prim_raw      = sm.prim_type;
        si.prim_method   = method_label;
        si.has_tex       = m_gpu_model->meshes[i].tex_id != 0;
        si.method_sel    = default_sel;
        m_ui_state.submeshes.push_back(si);
    }

    // ── Bind pose ──
    if ((int)model->bind_pose.size() >= N_BONES) {
        for (int i=0;i<N_BONES;++i) {
            m_bind_pose[i] = model->bind_pose[i];
            m_inv_bind[i]  = glm::inverse(m_bind_pose[i]);
            m_cur_pose[i]  = m_bind_pose[i];
        }
        for (int i=0;i<N_BONES;++i) m_skinning[i] = glm::mat4(1.f);
        m_has_bones=true;
        m_renderer.set_bone_matrices(m_skinning.data(), N_BONES);
    }

    // ── Skeleton ──
    std::string skel_path = (fs::path(path).parent_path()/"BLACK_CAT.dat").string();
    if (fs::exists(skel_path)) {
        Skeleton* sk = parse_skeleton(skel_path, path);
        if (sk) {
            m_skeleton.reset(sk);
            m_gpu_skel = m_renderer.upload_skeleton(sk);
            std::cout << "Skeleton: " << sk->bones.size() << " bones\n";
            for (int i = 0; i < (int)sk->bones.size(); ++i)
                std::cout << "  bone[" << i << "] par=" << sk->bones[i].parent
                          << " name=" << sk->bones[i].name << "\n";
        }
    }

    delete model;
    m_cam.reset();
    std::cout << "Loaded: " << path << "\n";
}

// ── Run ───────────────────────────────────────────────────────────────────────

void App::run() {
    m_last_frame = glfwGetTime();
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        glfwGetFramebufferSize(m_window,&m_w,&m_h);

        double now = glfwGetTime();

        // ── Fly cam WASD tick ─────────────────────────────────────────────────
        if (m_world_mode && m_cam.fly && !ImGui::GetIO().WantCaptureKeyboard) {
            unsigned keys = 0;
            if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) keys |= 1;
            if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) keys |= 2;
            if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) keys |= 4;
            if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) keys |= 8;
            if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) keys |= 16;
            if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) keys |= 32;
            if (keys) {
                float dt = (float)(now - m_last_frame);
                if (dt > 0.1f) dt = 0.1f;
                m_cam.fly_move(dt, keys);
            }
        }

        tick_animation(now);

        glViewport(0,0,m_w,m_h);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_renderer.sel_submesh = m_ui_state.sel_submesh;
        m_ui.draw(m_ui_state,m_ui_cb,
                  m_renderer.wireframe,m_renderer.show_grid,m_renderer.show_skel,
                  m_renderer.show_uv,m_h);

        // Rotation HUD
        if (m_rot_mode) {
            ImGui::SetNextWindowPos({(float)vp_x()+10,10});
            ImGui::SetNextWindowBgAlpha(0.65f);
            ImGui::Begin("##rot",nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
                ImGuiWindowFlags_NoMove|ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextColored({1.f,0.8f,0.2f,1.f},"ROTATE  R");
            if (m_sel_bone>=0&&m_skeleton)
                ImGui::Text("Bone: %s",m_skeleton->bones[m_sel_bone].name.c_str());
            else ImGui::Text("Model");
            std::string ax = m_rot_axis.x>0.5f?"X":m_rot_axis.y>0.5f?"Y":"Z";
            ImGui::Text("Axis: %s  Angle: %.1f°",ax.c_str(),glm::degrees(m_rot_accum));
            ImGui::TextDisabled("X/Y/Z constrain | LMB/Enter confirm | RMB/Esc cancel");
            ImGui::End();
        }

        // Fly cam HUD
        if (m_world_mode && m_cam.fly) {
            ImGui::SetNextWindowPos({(float)vp_x()+10, 10});
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::Begin("##flyhud", nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
                ImGuiWindowFlags_NoMove|ImGuiWindowFlags_AlwaysAutoResize|
                ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::TextColored({0.4f,1.f,0.6f,1.f}, "FLY CAM");
            ImGui::TextDisabled("WASD  move    Q/E  down/up");
            ImGui::TextDisabled("RMB   look    scroll  speed");
            ImGui::TextDisabled("Speed: %.0f u/s", m_cam.fly_speed);
            if (m_fly_look)
                ImGui::TextColored({1.f,0.8f,0.3f,1.f}, "LOOKING  (release RMB)");
            ImGui::End();
        }

        ImGui::Render();

        int w=vp_w(m_w);
        if (m_world_mode && !m_world_draws.empty()) {
            std::vector<std::pair<GPUModel*, glm::mat4>> draws;
            draws.reserve(m_world_draws.size());
            for (auto& dc : m_world_draws) draws.push_back(std::make_pair(dc.model, dc.xform));
            m_renderer.draw_world_instances(m_cam, vp_x(), w, m_h, draws);
        } else {
            m_renderer.draw_scene(m_cam,vp_x(),w,m_h,m_gpu_model,m_gpu_skel);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(m_window);
    }
}

void App::shutdown() {
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel; }
    for (auto& [k, gm] : m_world_gpu_cache) { if (gm) { gm->release(); delete gm; } }
    m_world_gpu_cache.clear();
    m_renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}
// ── Prim-type override ────────────────────────────────────────────────────────

static std::vector<uint32_t> reinterpret_strip(const std::vector<uint16_t>& raw, uint32_t vc) {
    std::vector<uint32_t> tris;
    int parity = 0;
    for (size_t i = 0; i + 2 < raw.size(); ++i) {
        uint32_t a=raw[i], b=raw[i+1], c=raw[i+2];
        if (a>=vc||b>=vc||c>=vc){ parity^=1; continue; }
        if (a==b||b==c||a==c)  { parity^=1; continue; }
        if (parity){ tris.push_back(a); tris.push_back(c); tris.push_back(b); }
        else       { tris.push_back(a); tris.push_back(b); tris.push_back(c); }
        parity ^= 1;
    }
    return tris;
}
static std::vector<uint32_t> reinterpret_trilist(const std::vector<uint16_t>& raw, uint32_t vc) {
    std::vector<uint32_t> tris;
    for (size_t i = 0; i + 2 < raw.size(); i += 3) {
        uint32_t a=raw[i], b=raw[i+1], c=raw[i+2];
        if (a>=vc||b>=vc||c>=vc) continue;
        if (a==b||b==c||a==c)   continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
    }
    return tris;
}
static std::vector<uint32_t> reinterpret_quadlist(const std::vector<uint16_t>& raw, uint32_t vc) {
    std::vector<uint32_t> tris;
    for (size_t i = 0; i + 3 < raw.size(); i += 4) {
        uint32_t a=raw[i], b=raw[i+1], c=raw[i+2], d_=raw[i+3];
        if (a>=vc||b>=vc||c>=vc||d_>=vc) continue;
        tris.push_back(a); tris.push_back(b); tris.push_back(c);
        tris.push_back(a); tris.push_back(c); tris.push_back(d_);
    }
    return tris;
}
static std::vector<uint32_t> reinterpret_trifan(const std::vector<uint16_t>& raw, uint32_t vc) {
    std::vector<uint32_t> tris;
    if (raw.size() < 3) return tris;
    uint32_t root = raw[0];
    if (root >= vc) return tris;
    for (size_t i = 1; i + 1 < raw.size(); ++i) {
        uint32_t b=raw[i], c=raw[i+1];
        if (b>=vc||c>=vc||root==b||b==c||root==c) continue;
        tris.push_back(root); tris.push_back(b); tris.push_back(c);
    }
    return tris;
}

void App::rebuild_prim_override(int smi, int sel) {
    if (!m_gpu_model) return;
    if (smi < 0 || smi >= (int)m_cached_raw.size()) return;
    auto& rd = m_cached_raw[smi];

    if (rd.raw.empty()) {
        // No raw index buffer — generate from sequential vertex order
        std::vector<uint32_t> tris;
        if (sel == 2) { // QuadList
            for (uint32_t i = 0; i + 3 < rd.vc; i += 4) {
                tris.push_back(i); tris.push_back(i+1); tris.push_back(i+2);
                tris.push_back(i); tris.push_back(i+2); tris.push_back(i+3);
            }
        } else if (sel == 3) { // TFan
            for (uint32_t i = 1; i + 1 < rd.vc; ++i) {
                tris.push_back(0); tris.push_back(i); tris.push_back(i+1);
            }
        } else if (sel == 1) { // TList — sequential groups of 3
            for (uint32_t i = 0; i + 2 < rd.vc; i += 3) {
                tris.push_back(i); tris.push_back(i+1); tris.push_back(i+2);
            }
        } else { // TStrip
            int parity = 0;
            for (uint32_t i = 0; i + 2 < rd.vc; ++i) {
                if (parity){ tris.push_back(i); tris.push_back(i+2); tris.push_back(i+1); }
                else       { tris.push_back(i); tris.push_back(i+1); tris.push_back(i+2); }
                parity ^= 1;
            }
        }
        m_gpu_model->update_mesh_indices(smi, tris);
        return;
    }

    std::vector<uint32_t> tris;
    switch (sel) {
        case 0: tris = reinterpret_strip   (rd.raw, rd.vc); break;
        case 1: tris = reinterpret_trilist  (rd.raw, rd.vc); break;
        case 2: tris = reinterpret_quadlist (rd.raw, rd.vc); break;
        case 3: tris = reinterpret_trifan   (rd.raw, rd.vc); break;
        default: return;
    }
    m_gpu_model->update_mesh_indices(smi, tris);
}