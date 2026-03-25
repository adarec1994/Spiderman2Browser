#include "App.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

// ── GLFW thunks ───────────────────────────────────────────

static App* g_app = nullptr;

void App::cb_key(GLFWwindow* w, int key, int, int action, int) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE)
        glfwSetWindowShouldClose(w, 1);
    if (action == GLFW_PRESS && key == GLFW_KEY_R)
        g_app->m_cam.reset();
}

void App::cb_mouse_btn(GLFWwindow*, int btn, int action, int) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double x,y; glfwGetCursorPos(g_app->m_window, &x, &y);
    // Only interact with viewport (right of panel)
    if (x < UI::PANEL_W) return;
    if (action == GLFW_PRESS) {
        if (btn == GLFW_MOUSE_BUTTON_LEFT)  { g_app->m_drag_l=true; g_app->m_lx=x; g_app->m_ly=y; }
        if (btn == GLFW_MOUSE_BUTTON_RIGHT) { g_app->m_drag_r=true; g_app->m_lx=x; g_app->m_ly=y; }
    } else {
        if (btn == GLFW_MOUSE_BUTTON_LEFT)  g_app->m_drag_l=false;
        if (btn == GLFW_MOUSE_BUTTON_RIGHT) g_app->m_drag_r=false;
    }
}

void App::cb_cursor(GLFWwindow*, double x, double y) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double dx = x - g_app->m_lx, dy = y - g_app->m_ly;
    g_app->m_lx = x; g_app->m_ly = y;
    if (g_app->m_drag_l) g_app->m_cam.orbit((float)dx, (float)dy);
    if (g_app->m_drag_r) g_app->m_cam.do_pan((float)dx, (float)dy);
}

void App::cb_scroll(GLFWwindow*, double, double dy) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double x,y; glfwGetCursorPos(g_app->m_window, &x, &y);
    if (x < UI::PANEL_W) return;
    g_app->m_cam.zoom((float)dy);
}

void App::cb_resize(GLFWwindow*, int w, int h) {
    g_app->m_w = w; g_app->m_h = h;
    glViewport(0,0,w,h);
}

// ── Init ──────────────────────────────────────────────────

bool App::init(int w, int h, const char* title) {
    g_app = this;
    m_w = w; m_h = h;

    if (!glfwInit()) { std::cerr << "GLFW init failed\n"; return false; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    m_window = glfwCreateWindow(w, h, title, nullptr, nullptr);
    if (!m_window) { std::cerr << "Window creation failed\n"; glfwTerminate(); return false; }
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD init failed\n"; return false;
    }

    // GLFW callbacks
    glfwSetKeyCallback(m_window,         cb_key);
    glfwSetMouseButtonCallback(m_window, cb_mouse_btn);
    glfwSetCursorPosCallback(m_window,   cb_cursor);
    glfwSetScrollCallback(m_window,      cb_scroll);
    glfwSetFramebufferSizeCallback(m_window, cb_resize);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.f;
    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    m_renderer.init();
    setup_callbacks();

    // Restore last folder and auto-scan
    std::string saved = UI::load_folder();
    if (!saved.empty() && fs::exists(saved)) {
        m_ui_state.folder = saved;
        scan_folder(saved);
    }

    std::cout << "XBX Viewer ready. OpenGL " << glGetString(GL_VERSION) << "\n";
    return true;
}

void App::setup_callbacks() {
    m_ui_cb.on_scan_folder = [this](const std::string& folder) {
        scan_folder(folder);
    };
    m_ui_cb.on_select_file = [this](int idx) {
        m_ui_state.selected = idx;
        load_file(idx);
    };
    m_ui_cb.on_reset_camera = [this]() {
        m_cam.reset();
    };
}

// ── Scan + Load ───────────────────────────────────────────

void App::scan_folder(const std::string& folder) {
    m_ui_state.files.clear();
    m_ui_state.selected = -1;
    m_ui_state.folder   = folder;
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(folder, ec)) {
        if (ec) break;
        auto p = entry.path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".xbx") m_ui_state.files.push_back(p.string());
    }
    std::sort(m_ui_state.files.begin(), m_ui_state.files.end());
    m_ui_state.status_msg = std::to_string(m_ui_state.files.size()) + " files found";
}

void App::load_file(int idx) {
    if (idx < 0 || idx >= (int)m_ui_state.files.size()) return;
    const std::string& path = m_ui_state.files[idx];

    // Free old GPU data
    if (m_gpu_model) { m_gpu_model->free(); delete m_gpu_model; m_gpu_model = nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->free();  delete m_gpu_skel;  m_gpu_skel  = nullptr; }
    m_ui_state.has_model = false;
    m_ui_state.mesh_info.clear();
    m_ui_state.status_msg = "Loading...";

    XBXModel* model = parse_xbx(path);
    if (!model) { m_ui_state.status_msg = "Error: Not XBXM or no geometry"; return; }

    m_gpu_model = m_renderer.upload_model(model);
    m_ui_state.has_model = true;
    m_ui_state.status_msg = "Loaded";

    for (int i = 0; i < (int)model->submeshes.size(); ++i) {
        auto& sm = model->submeshes[i];
        std::string tc = m_gpu_model->meshes[i].tex_id ? "(tex)" : "(no tex)";
        m_ui_state.mesh_info.push_back(
            "SM" + std::to_string(i) + ": " +
            std::to_string(sm.indices.size()/3) + " tris " + tc +
            "\n    " + sm.mat_name);
    }

    // Try loading skeleton from same folder
    std::string skel_path = (fs::path(path).parent_path() / "BLACK_CAT.dat").string();
    if (fs::exists(skel_path)) {
        Skeleton* sk = parse_skeleton(skel_path, path);
        if (sk) {
            m_gpu_skel = m_renderer.upload_skeleton(sk);
            delete sk;
            std::cout << "Skeleton loaded\n";
        }
    }

    delete model;
    m_cam.reset();
    std::cout << "Loaded: " << path << "\n";
}

// ── Main loop ─────────────────────────────────────────────

void App::run() {
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        glfwGetFramebufferSize(m_window, &m_w, &m_h);

        glViewport(0,0,m_w,m_h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // ImGui UI first — updates wireframe/grid/skel state
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_ui.draw(m_ui_state, m_ui_cb,
                  m_renderer.wireframe,
                  m_renderer.show_grid,
                  m_renderer.show_skel,
                  m_h);

        ImGui::Render();

        // 3-D scene (right of panel) — drawn with up-to-date state
        int vp_w = std::max(m_w - UI::PANEL_W, 400);
        m_renderer.draw_scene(m_cam, UI::PANEL_W, vp_w, m_h,
                              m_gpu_model, m_gpu_skel);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(m_window);
    }
}

void App::shutdown() {
    if (m_gpu_model) { m_gpu_model->free(); delete m_gpu_model; }
    if (m_gpu_skel)  { m_gpu_skel->free();  delete m_gpu_skel;  }
    m_renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}