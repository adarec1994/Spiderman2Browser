#pragma once
#include "Renderer.h"
#include "Camera.h"
#include "UI.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include <string>
#include <vector>
#include <memory>

struct GLFWwindow;

class App {
public:
    bool init(int w, int h, const char* title);
    void run();
    void shutdown();

private:
    GLFWwindow* m_window    = nullptr;
    int         m_w = 1280, m_h = 800;

    Renderer    m_renderer;
    Camera      m_cam;
    UI          m_ui;
    UIState     m_ui_state;
    UICallbacks m_ui_cb;

    GPUModel*    m_gpu_model = nullptr;
    GPUSkeleton* m_gpu_skel  = nullptr;

    // Skeleton CPU copy for picking and bone rotation
    std::unique_ptr<Skeleton> m_skeleton;

    // Bone selection
    int   m_sel_bone = -1;

    // Blender-style R-rotate mode
    // Works for both selected bone and the whole model (if no bone selected)
    bool      m_rot_mode   = false;
    double    m_rot_start_x = 0;   // screen x when R was pressed
    double    m_rot_start_y = 0;
    float     m_rot_accum  = 0.f;  // accumulated angle (radians)
    glm::vec3 m_rot_axis{0,1,0};   // world-space axis (default Y; X/Z constrained)
    // Saved bone poses for cancel (ESC)
    std::vector<glm::vec3> m_bone_pose_backup;
    // Model rotation (Euler Y) for whole-model R-rotate
    float     m_model_rot_y = 0.f;

    // Mouse drag state
    bool  m_drag_l = false, m_drag_r = false;
    double m_lx = 0, m_ly = 0;

    void load_file(int idx);
    void scan_folder(const std::string& folder);
    void setup_callbacks();

    // Picking
    int  pick_bone(double mx, double my);
    void try_select(double mx, double my);

    // Bone / model rotation
    void begin_rotate(double mx, double my);
    void update_rotate(double mx);
    void confirm_rotate();
    void cancel_rotate();
    void rebuild_skel_gpu();

    // GLFW callbacks
    static void cb_key       (GLFWwindow*, int, int, int, int);
    static void cb_mouse_btn (GLFWwindow*, int, int, int);
    static void cb_cursor    (GLFWwindow*, double, double);
    static void cb_scroll    (GLFWwindow*, double, double);
    static void cb_resize    (GLFWwindow*, int, int);
};