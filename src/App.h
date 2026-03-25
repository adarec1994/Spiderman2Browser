#pragma once
#include "Renderer.h"
#include "Camera.h"
#include "UI.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include <string>
#include <vector>

struct GLFWwindow;

class App {
public:
    bool init(int w, int h, const char* title);
    void run();
    void shutdown();

private:
    GLFWwindow* m_window = nullptr;
    int         m_w = 1280, m_h = 800;

    Renderer    m_renderer;
    Camera      m_cam;
    UI          m_ui;
    UIState     m_ui_state;
    UICallbacks m_ui_cb;

    GPUModel*    m_gpu_model = nullptr;
    GPUSkeleton* m_gpu_skel  = nullptr;

    // Mouse state
    bool  m_drag_l = false, m_drag_r = false;
    double m_lx = 0, m_ly = 0;

    void load_file(int idx);
    void scan_folder(const std::string& folder);
    void setup_callbacks();

    // GLFW callbacks (static thunks)
    static void cb_key(GLFWwindow*, int, int, int, int);
    static void cb_mouse_btn(GLFWwindow*, int, int, int);
    static void cb_cursor(GLFWwindow*, double, double);
    static void cb_scroll(GLFWwindow*, double, double);
    static void cb_resize(GLFWwindow*, int, int);
};
