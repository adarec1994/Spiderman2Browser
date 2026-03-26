#include "App.h"
#include "Texture.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <filesystem>
#include <algorithm>
#include <iostream>
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
                g_app->try_select(mx,my);
                g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
            }
        }
        if (btn == GLFW_MOUSE_BUTTON_RIGHT && in_vp) {
            if (g_app->m_rot_mode) g_app->cancel_rotate();
            else { g_app->m_drag_r=true; g_app->m_lx=mx; g_app->m_ly=my; }
        }
        if (btn == GLFW_MOUSE_BUTTON_MIDDLE && in_vp) {
            g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
        }
    } else {
        if (btn==GLFW_MOUSE_BUTTON_LEFT||btn==GLFW_MOUSE_BUTTON_MIDDLE) g_app->m_drag_l=false;
        if (btn==GLFW_MOUSE_BUTTON_RIGHT)                                g_app->m_drag_r=false;
    }
}

void App::cb_cursor(GLFWwindow*, double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double dx=mx-g_app->m_lx, dy=my-g_app->m_ly;
    g_app->m_lx=mx; g_app->m_ly=my;
    if (g_app->m_rot_mode) { g_app->update_rotate(mx); return; }
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
    m_ui_cb.on_play_anim    = [this](){
        if (m_anim_sel<0) return;
        m_anim_play = !m_anim_play;
        m_ui_state.anim_playing = m_anim_play;
        m_last_frame = glfwGetTime();
    };
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

    // Scan for animations in same folder
    load_animations(folder);
}

void App::load_file(int idx) {
    if (idx<0||idx>=(int)m_ui_state.files.size()) return;
    const std::string& path = m_ui_state.files[idx];

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
        si.mat_name    = sm.mat_name;
        si.tri_count   = (int)sm.indices.size()/3;
        si.prim_raw    = sm.prim_type;
        si.prim_method = method_label;
        si.has_tex     = m_gpu_model->meshes[i].tex_id != 0;
        si.method_sel  = default_sel;
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
        tick_animation(now);

        glViewport(0,0,m_w,m_h);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_renderer.sel_submesh = m_ui_state.sel_submesh;
        m_ui.draw(m_ui_state,m_ui_cb,
                  m_renderer.wireframe,m_renderer.show_grid,m_renderer.show_skel,m_h);

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

        ImGui::Render();

        int w=vp_w(m_w);
        m_renderer.draw_scene(m_cam,vp_x(),w,m_h,m_gpu_model,m_gpu_skel);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(m_window);
    }
}

void App::shutdown() {
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel; }
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