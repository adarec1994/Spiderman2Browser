#pragma once
#include "Renderer.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include <string>
#include <vector>
#include <functional>

struct SubmeshInfo {
    std::string  mat_name;
    int          tri_count   = 0;
    uint32_t     prim_raw    = 0;     // raw ptr+0x28 from file
    const char*  prim_method = "";    // human label for current interpretation
    bool         has_tex     = false;
    int          method_sel  = 0;     // current dropdown selection
};

struct UIState {
    std::string              folder;
    std::vector<std::string> files;
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
};

struct UICallbacks {
    std::function<void(const std::string&)>  on_scan_folder;
    std::function<void(int)>                 on_select_file;
    std::function<void()>                    on_reset_camera;
    std::function<void(int)>                 on_select_anim;
    std::function<void()>                    on_play_anim;
    std::function<void(int,int)>             on_prim_override; // (submesh_idx, dropdown_sel)
};

class UI {
public:
    static int PANEL_W;

    static void save_folder(const std::string& folder);
    static std::string load_folder();

    void draw(UIState& state, UICallbacks& cb,
              bool& wireframe, bool& show_grid, bool& show_skel,
              int win_h);
};