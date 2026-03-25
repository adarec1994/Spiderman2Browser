#pragma once
#include "Renderer.h"
#include "XBXParser.h"
#include "Skeleton.h"
#include <string>
#include <vector>
#include <functional>

struct UIState {
    std::string           folder;
    std::vector<std::string> files;
    int                   selected = -1;
    std::string           status_msg;
    bool                  has_model = false;
    std::vector<std::string> mesh_info;  // per-submesh info strings
};

// Callbacks the UI fires
struct UICallbacks {
    std::function<void(const std::string&)> on_scan_folder;
    std::function<void(int)>                on_select_file;
    std::function<void()>                   on_reset_camera;
};

class UI {
public:
    static int PANEL_W;  // globally adjustable panel width

    static void save_folder(const std::string& folder);
    static std::string load_folder();

    void draw(UIState& state, UICallbacks& cb,
              bool& wireframe, bool& show_grid, bool& show_skel,
              int win_h);
};