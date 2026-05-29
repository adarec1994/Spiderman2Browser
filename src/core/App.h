#pragma once
#include "Renderer.h"
#include "Camera.h"
#include "UI.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include "Animation.h"
#include "WorldParser.h"
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <unordered_map>
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
    std::array<glm::mat4, 60>  m_bind_pose;
    std::array<glm::mat4, 60>  m_inv_bind;
    std::array<glm::mat4, 60>  m_cur_pose;
    std::array<glm::mat4, 60>  m_skinning;
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
    bool   m_drag_l   = false, m_drag_r = false;
    double m_lx = 0,   m_ly   = 0;
    bool   m_fly_look = false;  // RMB held in world fly mode = mouse-look

    // ── Animation ─────────────────────────────────────────────────────────────
    std::vector<AnimClip> m_anim_clips;
    int                   m_anim_sel   = -1;
    bool                  m_anim_play  = false;
    float                 m_anim_time  = 0.f;
    double                m_last_frame = 0.0;

    void load_animations(const std::string& folder);
    void select_animation(int idx);
    void tick_animation(double now);
    void apply_animation_pose(float t);
    void extract_animation(int idx);
    void extract_all_animations();
    void extract_model(int idx);   // right-click: copy model .xbx + its skeleton .dat for inspection

    std::vector<int> m_anim_bone_map;
    glm::quat m_anim_root_ref{1, 0, 0, 0}; // shared standing-pelvis ref (D_source) for the global anchor
    bool      m_anim_global_ref_set = false;
    void ensure_global_root_ref();         // capture D_source once from an idle clip (clip-independent)
    void build_anim_bone_map(const AnimClip& clip);
    SkeletonAnimMeta m_skel_meta;            // per-skeleton bone count / rest pose / scales
    std::vector<glm::quat> m_full_rest_pose; // skeleton rest quats (fill for un-animated bones)

    // Prim-type override
    struct RawMeshData { std::vector<uint16_t> raw; uint32_t vc;
                         std::vector<glm::vec3> positions; };
    std::vector<RawMeshData> m_cached_raw;
    void rebuild_prim_override(int smi, int sel);

    // ── World / area ──────────────────────────────────────────────────────────
    std::unordered_map<std::string, GPUModel*>   m_world_gpu_cache;
    std::unordered_map<std::string, std::string> m_xbx_registry;
    // base stem -> first full stem (for O(1) base+digits lookup)
    // e.g. "s_trfflitea" -> "s_trfflitea_00000001"
    std::unordered_map<std::string, std::string> m_xbx_base_index;
    // underscore-suffix -> best matching stem
    // e.g. "strtlampb" -> "s_strtlampb_00000001"
    std::unordered_map<std::string, std::string> m_xbx_suffix_index;

    struct WorldDrawCall { GPUModel* model; glm::mat4 xform; };
    std::vector<WorldDrawCall> m_world_draws;
    bool m_world_mode = false;

    GPUModel* world_get_or_load_model(const std::string& asset_name);
    void      load_sector_terrain(const std::string& dat_path);
    void      build_world_draws(const WorldData& wd);
    void      recentre_camera_on_world();
    void      load_world(const std::string& dat_path);
    void      load_all_worlds();
    void      clear_world();
    // ─────────────────────────────────────────────────────────────────────────

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
