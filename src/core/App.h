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

    
    static constexpr int N_BONES = 60;
    std::array<glm::mat4, 60>  m_bind_pose;
    std::array<glm::mat4, 60>  m_inv_bind;
    std::array<glm::mat4, 60>  m_cur_pose;
    std::array<glm::mat4, 60>  m_skinning;
    bool                        m_has_bones = false;

    
    int   m_sel_bone = -1;

    
    bool      m_rot_mode    = false;
    double    m_rot_start_x = 0;
    float     m_rot_accum   = 0.f;
    glm::vec3 m_rot_axis{0,1,0};
    std::array<glm::mat4, 60> m_cur_pose_backup;

    
    float m_model_rot_y = 0.f;

    
    bool   m_drag_l   = false, m_drag_r = false;
    double m_lx = 0,   m_ly   = 0;
    bool   m_fly_look = false;  

    
    std::vector<AnimClip> m_anim_clips;
    int                   m_anim_sel   = -1;
    bool                  m_anim_play  = false;
    float                 m_anim_time  = 0.f;
    double                m_last_frame = 0.0;

    void load_animations(const std::string& folder);
    void filter_animations_for_model(const std::string& model_path);
    void select_animation(int idx);
    void tick_animation(double now);
    void apply_animation_pose(float t);
    void extract_animation(int idx);
    void extract_all_animations();
    void extract_model(int idx);   

    std::vector<int> m_anim_bone_map;
    glm::quat m_anim_root_ref{1, 0, 0, 0}; 
    bool      m_anim_global_ref_set = false;
    bool      m_minion_lizard_model = false;
    void ensure_global_root_ref();         
    void build_anim_bone_map(const AnimClip& clip);
    SkeletonAnimMeta m_skel_meta;            
    std::vector<glm::quat> m_full_rest_pose; 

    struct RawMeshData { std::vector<uint16_t> raw; uint32_t vc;
                         std::vector<glm::vec3> positions; };
    std::vector<RawMeshData> m_cached_raw;

    
    std::unordered_map<std::string, GPUModel*>   m_world_gpu_cache;
    std::unordered_map<std::string, std::string> m_xbx_registry;
    
    
    std::unordered_map<std::string, std::string> m_xbx_base_index;
    
    
    std::unordered_map<std::string, std::string> m_xbx_suffix_index;

    struct WorldDrawCall { GPUModel* model; glm::mat4 xform; unsigned int tex_override = 0; };
    std::vector<WorldDrawCall> m_world_draws;
    
    
    
    
    
    InstancedWorld m_instanced_world;
    
    
    
    glm::vec3 m_world_bb_min{ 1e9f}, m_world_bb_max{-1e9f};
    bool m_world_mode = false;
    
    
    
    
    
    bool m_pending_load_all = false;

    GPUModel* world_get_or_load_model(const std::string& asset_name);
    void      load_sector_terrain(const std::string& dat_path);
    void      build_world_draws(const WorldData& wd);
    
    
    
    void      finalize_world_merge();
    void      recentre_camera_on_world();
    void      load_world(const std::string& dat_path);
    void      load_all_worlds();
    void      clear_world();
    
    
    
    void      pump_loading_frame(const char* label, float frac);
    

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
