static constexpr int ANIM_W = 220;
static constexpr const char* APP_NAME = "Spiderman 2 Asset Browser";

static const char* CFG_PATH = "spiderman_2_asset_browser.cfg";
static const char* LEGACY_CFG_PATH = "xbx_viewer.cfg";
static int s_pending_model_export_idx = -1;
static std::string s_pending_model_export_format;

static std::string export_dialog_start_dir(const std::string& model_path, const std::string& folder) {
    std::error_code ec;
    fs::path p = fs::path(model_path).parent_path();
    if (!p.empty() && fs::is_directory(p, ec)) return p.string();
    if (!folder.empty() && fs::is_directory(folder, ec)) return folder;
    return fs::current_path().string();
}

static void open_model_export_dialog(int idx, const std::string& fmt, UIState& state) {
    if (idx < 0 || idx >= (int)state.files.size()) return;
    s_pending_model_export_idx = idx;
    s_pending_model_export_format = fmt;
    IGFD::FileDialogConfig cfg;
    cfg.path = export_dialog_start_dir(state.files[idx], state.folder);
    cfg.fileName = fs::path(state.files[idx]).stem().string() + "." + fmt;
    cfg.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_ConfirmOverwrite;
    std::string filters = "." + fmt;
    std::string title = "Export model to " + fmt;
    ImGuiFileDialog::Instance()->OpenDialog("MODEL_EXPORT", title.c_str(), filters.c_str(), cfg);
}

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static std::string with_extension(std::string path, const std::string& fmt) {
    fs::path p(path);
    std::string ext = lower_copy(p.extension().string());
    std::string want = "." + fmt;
    if (ext != want) p.replace_extension(want);
    return p.string();
}



void UI::save_config(const std::string& xiso_path, const std::string& folder) {
    std::ofstream o(CFG_PATH);
    if (!o) return;
    if (!xiso_path.empty()) o << "xiso=" << xiso_path << "\n";
    if (!folder.empty())    o << "folder=" << folder << "\n";
}

void UI::load_config(std::string& xiso_path, std::string& folder) {
    xiso_path.clear();
    folder.clear();
    std::ifstream f(CFG_PATH);
    if (!f) {
        f.clear();
        f.open(LEGACY_CFG_PATH);
    }
    if (!f) return;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { first = false; continue; }

        auto eq = line.find('=');
        if (eq == std::string::npos) {
            
            if (first) folder = line;
            first = false;
            continue;
        }
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if      (k == "xiso")   xiso_path = v;
        else if (k == "folder") folder    = v;
        first = false;
    }
}

void UI::save_folder(const std::string& f)     { save_config("", f); }
std::string UI::load_folder() {
    std::string x, f; load_config(x, f); return f;
}




void UI::draw_splash(UIState& state, UICallbacks& cb) {
    float dw = ImGui::GetIO().DisplaySize.x;
    float dh = ImGui::GetIO().DisplaySize.y;

    
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({dw, dh});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.10f, 1.0f));
    ImGui::Begin("##splash_bg", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    
    const float btn_w = std::min(360.f, dw * 0.6f);
    const float btn_h = 40.f;
    ImGui::SetCursorPos({(dw - btn_w) * 0.5f, (dh - btn_h) * 0.5f});

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.45f, 0.80f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.40f, 0.75f, 1.0f));
    if (ImGui::Button("Browse to Spiderman2 ISO", {btn_w, btn_h})) {
        IGFD::FileDialogConfig cfg;
        cfg.path  = state.xiso_path.empty()
                      ? (state.folder.empty() ? "." : state.folder)
                      : fs::path(state.xiso_path).parent_path().string();
        cfg.flags = ImGuiFileDialogFlags_Modal;
        ImGuiFileDialog::Instance()->OpenDialog("SPLASH_XISO",
            "Browse to Spiderman2 ISO", ".iso,.xiso,.*", cfg);
    }
    ImGui::PopStyleColor(3);

    ImGui::End();
    ImGui::PopStyleColor();

    
    float fdw = std::min(720.f, dw * 0.8f);
    float fdh = std::min(480.f, dh * 0.75f);

    ImGui::SetNextWindowPos({(dw - fdw) * 0.5f, (dh - fdh) * 0.5f}, ImGuiCond_Always);
    if (ImGuiFileDialog::Instance()->Display("SPLASH_XISO",
            ImGuiWindowFlags_NoCollapse, {fdw, fdh}, {fdw, fdh})) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
            if (cb.on_select_xiso) cb.on_select_xiso(path);
        }
        ImGuiFileDialog::Instance()->Close();
    }
}
