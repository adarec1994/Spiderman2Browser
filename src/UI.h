#pragma once
#include "Renderer.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include <string>
#include <vector>
#include <functional>

struct SubmeshInfo {
    std::string  mat_name;
    std::string  shader_type;              // e.g. "smsimple", "smtranslucent"
    std::vector<std::string> tex_candidates; // all resolved candidates from parser
    int          tex_sel    = 0;           // current tex dropdown selection
    int          tri_count   = 0;
    uint32_t     prim_raw    = 0;
    const char*  prim_method = "";
    bool         has_tex     = false;
    int          method_sel  = 0;
};

struct UIState {
    std::string              folder;
    std::vector<std::string> files;        // .xbx model files
    std::vector<std::string> world_files;  // bare .dat world area files
    int                      selected    = -1;
    int                      sel_submesh = -1;
    std::string              status_msg;
    bool                     has_model   = false;
    std::vector<SubmeshInfo> submeshes;

    // Animation
    std::vector<std::string> anim_names;
    int                      anim_sel     = -1;
    bool                     anim_playing = false;
    float                    anim_time    = 0.f;
    float                    anim_dur     = 0.f;

    // Material editor
    bool mat_editor_open = false;
    char tex_filter[128] = {};
    std::vector<std::pair<std::string,std::string>> all_tex_entries;

    // Texture preview
    unsigned int preview_tex_id  = 0;
    std::string  preview_tex_name;
    bool         show_tex_preview = false;

    // World mode
    bool        world_mode           = false;
    int         world_instance_count = 0;
    int         world_prop_count     = 0;
    std::string world_dat_path;          // last loaded dat path (or "(all)")

    // World loading progress  (0..1, negative = idle)
    float       world_load_progress  = -1.f;
    std::string world_load_status;
};

struct UICallbacks {
    std::function<void(const std::string&)>      on_scan_folder;
    std::function<void(int)>                     on_select_file;
    std::function<void()>                        on_reset_camera;
    std::function<void(int)>                     on_select_anim;
    std::function<void()>                        on_play_anim;
    std::function<void(int,int)>                 on_prim_override;
    std::function<void(int,int)>                 on_tex_override;
    std::function<void(int, const std::string&)> on_tex_assign;
    std::function<void(int)>                     on_load_world_file;  // single dat by index
    std::function<void()>                        on_load_all_worlds;  // all world dats
};

class UI {
public:
    static int PANEL_W;

    static void save_folder(const std::string& folder);
    static std::string load_folder();

    void draw(UIState& state, UICallbacks& cb,
              bool& wireframe, bool& show_grid, bool& show_skel, bool& show_uv,
              int win_h);
    void draw_mat_editor(UIState& state, UICallbacks& cb);
};