#pragma once
#include "Renderer.h"
#include "Camera.h"
#include "UI.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <glm/glm.hpp>

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

    std::unique_ptr<Skeleton>  m_skeleton;

    // Skinning matrices
    static constexpr int N_BONES = 60;
    std::array<glm::mat4, 60>  m_bind_pose;    // from XBX, column-major
    std::array<glm::mat4, 60>  m_inv_bind;     // inverse of bind pose
    std::array<glm::mat4, 60>  m_cur_pose;     // current animated pose
    std::array<glm::mat4, 60>  m_skinning;     // cur_pose[i] * inv_bind[i]
    bool                        m_has_bones = false;

    // Selection
    int   m_sel_bone = -1;

    // Rotate mode (Blender-style R)
    bool      m_rot_mode    = false;
    double    m_rot_start_x = 0;
    float     m_rot_accum   = 0.f;
    glm::vec3 m_rot_axis{0,1,0};
    std::array<glm::mat4, 60> m_cur_pose_backup;

    // Whole-model rotation
    float m_model_rot_y = 0.f;

    // Mouse
    bool   m_drag_l = false, m_drag_r = false;
    double m_lx = 0, m_ly = 0;

    void load_file(int idx);
    void scan_folder(const std::string& folder);
    void setup_callbacks();

    void compute_skinning();
    void upload_skinning();

    int  pick_bone(double mx, double my);
    void try_select(double mx, double my);

    void begin_rotate(double mx);
    void update_rotate(double mx);
    void confirm_rotate();
    void cancel_rotate();
    void rebuild_skel_gpu();

    static void cb_key       (GLFWwindow*,int,int,int,int);
    static void cb_mouse_btn (GLFWwindow*,int,int,int);
    static void cb_cursor    (GLFWwindow*,double,double);
    static void cb_scroll    (GLFWwindow*,double,double);
    static void cb_resize    (GLFWwindow*,int,int);
};