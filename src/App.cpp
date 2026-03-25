#include "App.h"
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

static int vp_x()     { return UI::PANEL_W; }
static int vp_w(int W){ return std::max(W - UI::PANEL_W, 400); }

// ── Skinning ──────────────────────────────────────────────

void App::compute_skinning() {
    for (int i = 0; i < N_BONES; ++i)
        m_skinning[i] = m_cur_pose[i] * m_inv_bind[i];
}

void App::upload_skinning() {
    compute_skinning();
    m_renderer.set_bone_matrices(m_skinning.data(), N_BONES);
}

// ── Picking ───────────────────────────────────────────────

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
        float b  = glm::dot(oc,rd);
        float r  = pick_r/s;
        float disc = b*b - glm::dot(oc,oc) + r*r;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t > 0 && t < best_t) { best_t=t; best_i=i; }
    }
    return best_i;
}

void App::try_select(double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    int b = pick_bone(mx,my);
    m_sel_bone = b;
    m_renderer.sel_bone = b;
    if (b >= 0) std::cout << "Bone " << b << ": " << m_skeleton->bones[b].name << "\n";
}

// ── Rotation ──────────────────────────────────────────────

void App::begin_rotate(double mx) {
    if (!m_gpu_model) return;
    m_rot_mode    = true;
    m_rot_start_x = mx;
    m_rot_accum   = 0.f;
    m_rot_axis    = {0,1,0}; // default Y

    if (m_sel_bone >= 0 && m_has_bones)
        m_cur_pose_backup = m_cur_pose;
}

void App::update_rotate(double mx) {
    if (!m_rot_mode) return;
    float angle = (float)(mx - m_rot_start_x) / (float)m_w * glm::two_pi<float>();
    float delta = angle - m_rot_accum;
    m_rot_accum  = angle;

    glm::mat4 R = glm::rotate(glm::mat4(1), delta, m_rot_axis);

    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        // Pivot = selected bone's current world position
        glm::vec3 pivot = glm::vec3(m_cur_pose[m_sel_bone][3]);

        // Mark descendants
        int nb = (int)m_skeleton->bones.size();
        std::vector<bool> aff(nb, false);
        aff[m_sel_bone] = true;
        for (int i = 0; i < nb; ++i)
            if (m_skeleton->bones[i].parent >= 0 && aff[m_skeleton->bones[i].parent])
                aff[i] = true;

        for (int i = 0; i < nb; ++i) {
            if (!aff[i]) continue;
            // Rotate orientation
            glm::mat4& M = m_cur_pose[i];
            glm::vec3 t = glm::vec3(M[3]);
            M = R * M;
            // Rotate translation around pivot
            M[3] = glm::vec4(glm::vec3(R * glm::vec4(t - pivot, 0.f)) + pivot, 1.f);
        }

        // Also move bone visual positions for the skeleton overlay
        for (int i = 0; i < nb; ++i) {
            if (!aff[i]) continue;
            m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);
        }

        upload_skinning();
        rebuild_skel_gpu();

    } else {
        // Rotate whole model
        m_model_rot_y += delta;
        m_renderer.model_rot_y = m_model_rot_y;
    }
}

void App::confirm_rotate() {
    m_rot_mode = false;
}

void App::cancel_rotate() {
    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        m_cur_pose = m_cur_pose_backup;
        // Restore bone positions
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

// ── GLFW thunks ───────────────────────────────────────────

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
        if (btn==GLFW_MOUSE_BUTTON_RIGHT) g_app->m_drag_r=false;
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

// ── Init ──────────────────────────────────────────────────

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
}

// ── Scan + Load ───────────────────────────────────────────

void App::scan_folder(const std::string& folder) {
    m_ui_state.files.clear();
    m_ui_state.selected=-1;
    m_ui_state.folder=folder;
    std::error_code ec;
    for (auto& e:fs::recursive_directory_iterator(folder,ec)) {
        if (ec) break;
        auto p=e.path();
        std::string ext=p.extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        if (ext==".xbx") m_ui_state.files.push_back(p.string());
    }
    std::sort(m_ui_state.files.begin(),m_ui_state.files.end());
    m_ui_state.status_msg=std::to_string(m_ui_state.files.size())+" files found";
}

void App::load_file(int idx) {
    if (idx<0||idx>=(int)m_ui_state.files.size()) return;
    const std::string& path = m_ui_state.files[idx];

    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; m_gpu_model=nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel;  m_gpu_skel=nullptr; }
    m_skeleton.reset();
    m_has_bones=false;
    m_ui_state.has_model=false;
    m_ui_state.mesh_info.clear();
    m_ui_state.status_msg="Loading...";
    m_sel_bone=-1; m_renderer.sel_bone=-1;
    m_model_rot_y=0.f; m_renderer.model_rot_y=0.f;

    XBXModel* model = parse_xbx(path);
    if (!model) { m_ui_state.status_msg="Error: Not XBXM or no geometry"; return; }

    m_gpu_model = m_renderer.upload_model(model);
    m_ui_state.has_model=true;
    m_ui_state.status_msg="Loaded";

    for (int i=0;i<(int)model->submeshes.size();++i) {
        auto& sm=model->submeshes[i];
        m_ui_state.mesh_info.push_back(
            "SM"+std::to_string(i)+": "+std::to_string(sm.indices.size()/3)+" tris "+
            (m_gpu_model->meshes[i].tex_id?"(tex)":"(no tex)")+
            "\n    "+sm.mat_name);
    }

    // ── Load bind pose matrices ──
    if ((int)model->bind_pose.size() >= N_BONES) {
        for (int i=0;i<N_BONES;++i) {
            m_bind_pose[i] = model->bind_pose[i];
            m_inv_bind[i]  = glm::inverse(m_bind_pose[i]);
            m_cur_pose[i]  = m_bind_pose[i];
        }
        // Skinning matrices start as identity (no deformation at bind pose)
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
        }
    }

    delete model;
    m_cam.reset();
    std::cout << "Loaded: " << path << "\n";
}

// ── Run ───────────────────────────────────────────────────

void App::run() {
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        glfwGetFramebufferSize(m_window,&m_w,&m_h);

        glViewport(0,0,m_w,m_h);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_ui.draw(m_ui_state,m_ui_cb,
                  m_renderer.wireframe,m_renderer.show_grid,m_renderer.show_skel,m_h);

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
            ImGui::TextDisabled("X/Y/Z  constrain | LMB/Enter confirm | RMB/Esc cancel");
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