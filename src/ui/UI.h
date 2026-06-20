#pragma once
#include "Renderer.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include <string>
#include <vector>
#include <functional>

struct SubmeshInfo {
    std::string  mat_name;
    std::string  shader_type;              
    std::vector<std::string> tex_candidates; 
    int          tex_sel    = 0;           
    int          tri_count   = 0;
    uint32_t     prim_raw    = 0;
    const char*  prim_method = "";
    bool         has_tex     = false;
    int          method_sel  = 0;
};

struct UIState {
    std::string              folder;
    std::string              xiso_path;     
    std::vector<std::string> files;        
    std::vector<std::string> world_files;  
    int                      selected    = -1;
    int                      sel_submesh = -1;
    std::string              status_msg;
    bool                     has_model   = false;
    std::vector<SubmeshInfo> submeshes;

    
    bool                     show_splash = false;

    
    std::vector<std::string> anim_names;
    int                      anim_sel     = -1;
    bool                     anim_playing = false;
    float                    anim_time    = 0.f;
    float                    anim_dur     = 0.f;

    
    bool mat_editor_open = false;
    char tex_filter[128] = {};
    std::vector<std::pair<std::string,std::string>> all_tex_entries;

    
    unsigned int preview_tex_id  = 0;
    std::string  preview_tex_name;
    bool         show_tex_preview = false;

    
    bool        world_mode           = false;
    int         world_instance_count = 0;
    int         world_prop_count     = 0;
    std::string world_dat_path;          

    
    float       world_load_progress  = -1.f;
    std::string world_load_status;
};

struct UICallbacks {
    std::function<void(const std::string&)>      on_scan_folder;
    std::function<void(const std::string&)>      on_select_xiso; 
    std::function<void(int)>                     on_select_file;
    std::function<void()>                        on_reset_camera;
    std::function<void(int)>                     on_select_anim;
    std::function<void()>                        on_play_anim;
    std::function<void(int)>                     on_extract_anim;
    std::function<void()>                        on_extract_all_anims;
    std::function<void(int)>                     on_extract_model;
    std::function<void(int, const std::string&, const std::string&)> on_export_model;
    std::function<void(int, const std::string&)> on_tex_assign;
    std::function<void(int)>                     on_load_world_file;  
    std::function<void()>                        on_load_all_worlds;  
};

class UI {
public:
    static int PANEL_W;

    
    
    
    static void save_config(const std::string& xiso_path, const std::string& folder);
    static void load_config(std::string& xiso_path, std::string& folder);

    
    static void save_folder(const std::string& folder);
    static std::string load_folder();

    void draw(UIState& state, UICallbacks& cb,
              bool& wireframe, bool& show_grid, bool& show_skel, bool& show_uv,
              int win_h);
    void draw_splash(UIState& state, UICallbacks& cb);
    void draw_mat_editor(UIState& state, UICallbacks& cb);
};
