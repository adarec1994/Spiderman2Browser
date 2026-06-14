#include "UI.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <algorithm>

namespace fs = std::filesystem;

int UI::PANEL_W = 300;
static constexpr int ANIM_W = 220;
static constexpr const char* APP_NAME = "Spiderman 2 Asset Browser";

static const char* CFG_PATH = "spiderman_2_asset_browser.cfg";
static const char* LEGACY_CFG_PATH = "xbx_viewer.cfg";




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


void UI::draw(UIState& state, UICallbacks& cb,
              bool& wireframe, bool& show_grid, bool& show_skel, bool& show_uv,
              int) {

    float dh = ImGui::GetIO().DisplaySize.y;
    float dw = ImGui::GetIO().DisplaySize.x;
    const bool show_anim_panel = state.has_model && !state.anim_names.empty();

    
    if (state.show_splash) { draw_splash(state, cb); return; }

    float line = ImGui::GetTextLineHeightWithSpacing();
    float sy   = ImGui::GetStyle().ItemSpacing.y;
    float fpy  = ImGui::GetStyle().FramePadding.y;

    
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)PANEL_W, dh});
    ImGui::Begin("##panel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoBringToFrontOnFocus);

    
    ImGui::TextColored({0.6f, 0.8f, 1.f, 1.f}, "%s", APP_NAME);
    ImGui::Separator();

    
    
    const float opts_h = line * 2.f + fpy * 4 + sy * 3 + 32.f;
    
    const bool show_sm = state.has_model && !state.submeshes.empty();
    float sm_h  = show_sm ? std::clamp(dh * 0.28f, 120.f, 260.f) : 0.f;
    
    float used  = opts_h + (show_sm ? sm_h + sy * 2 + 6.f : 0.f) + sy * 2 + 6.f;
    float tabs_h = std::max(120.f, ImGui::GetContentRegionAvail().y - used);

    
    ImGui::BeginChild("##tabs_section", {0, tabs_h}, false);
    if (ImGui::BeginTabBar("##tabs")) {

        
        if (ImGui::BeginTabItem("Models")) {
            static char search[128] = {};
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##search", "Search...", search, sizeof(search));

            std::string search_lo = search;
            std::transform(search_lo.begin(), search_lo.end(), search_lo.begin(), ::tolower);

            std::vector<int> filtered;
            filtered.reserve(state.files.size());
            for (int i = 0; i < (int)state.files.size(); ++i) {
                if (search_lo.empty()) { filtered.push_back(i); continue; }
                std::string fn = fs::path(state.files[i]).filename().string();
                std::transform(fn.begin(), fn.end(), fn.begin(), ::tolower);
                if (fn.find(search_lo) != std::string::npos) filtered.push_back(i);
            }

            ImGui::BeginChild("##files_scroll", {0, 0}, false);
            for (int i = 0; i < (int)filtered.size(); ++i) {
                int idx = filtered[i];
                std::string lbl = fs::path(state.files[idx]).filename().string()
                                  + "##f" + std::to_string(idx);
                if (ImGui::Selectable(lbl.c_str(), idx == state.selected) && idx != state.selected)
                    cb.on_select_file(idx);
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Extract model")) {
                        if (cb.on_extract_model) cb.on_extract_model(idx);
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        
        if (ImGui::BeginTabItem("World")) {
            int n_world = (int)state.world_files.size();

            if (state.world_load_progress >= 0.f) {
                char overlay[64];
                snprintf(overlay, sizeof(overlay), "Loading... %s",
                         state.world_load_status.c_str());
                ImGui::ProgressBar(state.world_load_progress, {-1, 0}, overlay);
            } else if (state.world_mode) {
                ImGui::TextColored({0.4f, 1.f, 0.6f, 1.f}, "WORLD ACTIVE");
                ImGui::TextDisabled("%d instances  %d props",
                    state.world_instance_count, state.world_prop_count);
                if (!state.world_dat_path.empty()) {
                    std::string wfn = state.world_dat_path;
                    auto sl = wfn.rfind('/');
                    if (sl == std::string::npos) sl = wfn.rfind('\\');
                    if (sl != std::string::npos) wfn = wfn.substr(sl + 1);
                    ImGui::TextDisabled("%s", wfn.c_str());
                }
            }

            if (n_world > 0) {
                if (ImGui::Button("Load All", {-1, 0}))
                    if (cb.on_load_all_worlds) cb.on_load_all_worlds();
                ImGui::Separator();
            }

            static char wsearch[128] = {};
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##wsearch", "Search...", wsearch, sizeof(wsearch));
            std::string wlo = wsearch;
            std::transform(wlo.begin(), wlo.end(), wlo.begin(), ::tolower);

            ImGui::BeginChild("##worlds_scroll", {0, 0}, false);
            for (int i = 0; i < n_world; ++i) {
                std::string fn = fs::path(state.world_files[i]).filename().string();
                if (!wlo.empty()) {
                    std::string fnl = fn;
                    std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
                    if (fnl.find(wlo) == std::string::npos) continue;
                }
                bool active = state.world_mode &&
                              state.world_dat_path == state.world_files[i];
                if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.4f, 1.f, 0.6f, 1.f});
                std::string lbl = fn + "##w" + std::to_string(i);
                if (ImGui::Selectable(lbl.c_str(), active))
                    if (cb.on_load_world_file) cb.on_load_world_file(i);
                if (active) ImGui::PopStyleColor();
            }
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    
    if (show_sm) {
        ImGui::Separator();
        ImGui::BeginChild("##submesh_section", {0, sm_h}, false);
        ImGui::TextColored({0.6f, 0.8f, 1.f, 1.f}, "Submeshes (%d)",
                           (int)state.submeshes.size());

        ImGui::BeginChild("##sm_scroll", {0, 0}, false);
        for (int i = 0; i < (int)state.submeshes.size(); ++i) {
            auto& sm = state.submeshes[i];
            ImGui::PushID(i);
            bool sel = (state.sel_submesh == i);
            ImGui::TextColored(sel ? ImVec4{0.3f, 1.f, 0.4f, 1.f}
                                   : ImVec4{0.8f, 0.8f, 0.8f, 1.f},
                "SM%d", i);
            ImGui::SameLine();
            if (ImGui::Selectable(sm.mat_name.c_str(), sel))
                state.sel_submesh = i;
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    
    ImGui::Separator();
    ImGui::BeginChild("##opts_section", {0, 0}, false);
    ImGui::Checkbox("Wireframe", &wireframe);
    ImGui::SameLine(); ImGui::Checkbox("Grid", &show_grid);
    ImGui::Checkbox("Skeleton", &show_skel);
    ImGui::SameLine(); ImGui::Checkbox("UV", &show_uv);
    if (ImGui::Button("Reset Camera")) cb.on_reset_camera();
    if (state.preview_tex_id) {
        ImGui::SameLine();
        if (ImGui::Button("Tex Preview"))
            state.show_tex_preview = !state.show_tex_preview;
    }

    ImGui::EndChild();

    ImGui::End();

    if (state.mat_editor_open)
        draw_mat_editor(state, cb);

    
    if (state.show_tex_preview && state.preview_tex_id) {
        ImGui::SetNextWindowSize({300.f, 340.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({dw * 0.5f - 150.f, dh * 0.5f - 170.f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Texture Preview", &state.show_tex_preview)) {
            ImGui::TextUnformatted(state.preview_tex_name.c_str());
            ImGui::Separator();
            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::Image((ImTextureID)(uintptr_t)state.preview_tex_id, {avail, avail});
        }
        ImGui::End();
    }

    
    if (show_anim_panel) {
        ImGui::SetNextWindowPos({dw - ANIM_W, 0});
        ImGui::SetNextWindowSize({(float)ANIM_W, dh});
        ImGui::Begin("##anim", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored({1.f, 0.8f, 0.4f, 1.f}, "Animations");
        ImGui::TextDisabled("%d clips", (int)state.anim_names.size());
        ImGui::Separator();

        if (state.anim_sel >= 0) {
            const char* btn_lbl = state.anim_playing ? "Stop##pb" : "Play##pb";
            if (ImGui::Button(btn_lbl)) {
                if (cb.on_play_anim) cb.on_play_anim();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset##tr")) {
                state.anim_time    = 0.f;
                state.anim_playing = false;
                if (cb.on_select_anim) cb.on_select_anim(state.anim_sel);
            }

            float prog = (state.anim_dur > 0) ? (state.anim_time / state.anim_dur) : 0.f;
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%.2f / %.2fs", state.anim_time, state.anim_dur);
            ImGui::ProgressBar(prog, {-1, 0}, overlay);
            ImGui::Separator();
        }

        float anim_list_h = dh - ImGui::GetCursorPosY() - ImGui::GetStyle().WindowPadding.y;
        ImGui::BeginChild("##cliplist", {0, anim_list_h}, ImGuiChildFlags_Borders);
        ImGuiListClipper aclip;
        aclip.Begin((int)state.anim_names.size());
        while (aclip.Step())
            for (int i = aclip.DisplayStart; i < aclip.DisplayEnd; ++i) {
                bool sel = (i == state.anim_sel);
                if (ImGui::Selectable(state.anim_names[i].c_str(), sel)) {
                    state.anim_sel = i;
                    if (cb.on_select_anim) cb.on_select_anim(i);
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Extract to animations")) {
                        if (cb.on_extract_anim) cb.on_extract_anim(i);
                    }
                    ImGui::EndPopup();
                }
            }
        aclip.End();
        if (ImGui::BeginPopupContextWindow("##animlist_ctx",
                                           ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("Extract all to animations", nullptr, false,
                                !state.anim_names.empty())) {
                if (cb.on_extract_all_anims) cb.on_extract_all_anims();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();

        ImGui::End();
    }
}

void UI::draw_mat_editor(UIState& state, UICallbacks& cb) {
    if (state.sel_submesh < 0 || state.sel_submesh >= (int)state.submeshes.size()) {
        state.mat_editor_open = false;
        return;
    }
    auto& sm = state.submeshes[state.sel_submesh];

    ImGuiIO& io = ImGui::GetIO();
    float dw = io.DisplaySize.x, dh = io.DisplaySize.y;

    ImGui::SetNextWindowSize({900.f, 620.f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({dw*0.5f - 450.f, dh*0.5f - 310.f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material Editor", &state.mat_editor_open,
                      ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::End(); return;
    }

    float total_w = ImGui::GetContentRegionAvail().x;
    float browser_w = 260.f;
    float canvas_w  = total_w - browser_w - 8.f;
    float canvas_h  = ImGui::GetContentRegionAvail().y;

    
    ImGui::BeginChild("##matcanvas", {canvas_w, canvas_h}, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      orig = ImGui::GetWindowPos();
    ImVec2      csz  = ImGui::GetWindowSize();

    
    ImU32 bg    = IM_COL32(28,28,32,255);
    ImU32 gridC = IM_COL32(45,45,52,255);
    dl->AddRectFilled(orig, {orig.x+csz.x, orig.y+csz.y}, bg);
    for (float x = orig.x; x < orig.x+csz.x; x += 32) dl->AddLine({x, orig.y}, {x, orig.y+csz.y}, gridC);
    for (float y = orig.y; y < orig.y+csz.y; y += 32) dl->AddLine({orig.x, y}, {orig.x+csz.x, y}, gridC);

    
    const float NW = 180.f, NH = 90.f, ROUNDING = 6.f;
    ImVec2 shader_pos = {orig.x + 40.f, orig.y + canvas_h*0.5f - NH*0.5f};
    ImU32 shader_hdr  = IM_COL32(55,100,60,255);
    ImU32 shader_body = IM_COL32(38,50,38,255);
    ImU32 border      = IM_COL32(80,140,80,200);

    dl->AddRectFilled(shader_pos, {shader_pos.x+NW, shader_pos.y+26}, shader_hdr, ROUNDING, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled({shader_pos.x, shader_pos.y+26}, {shader_pos.x+NW, shader_pos.y+NH}, shader_body, ROUNDING, ImDrawFlags_RoundCornersBottom);
    dl->AddRect(shader_pos, {shader_pos.x+NW, shader_pos.y+NH}, border, ROUNDING);
    dl->AddText({shader_pos.x+8, shader_pos.y+6}, IM_COL32(200,255,200,255), "Shader");
    const char* sh_label = sm.shader_type.empty() ? "(none)" : sm.shader_type.c_str();
    dl->AddText({shader_pos.x+8, shader_pos.y+32}, IM_COL32(180,220,180,255), sh_label);
    dl->AddText({shader_pos.x+8, shader_pos.y+50}, IM_COL32(130,150,130,255), sm.mat_name.c_str());
    
    ImVec2 out_sock = {shader_pos.x+NW, shader_pos.y+NH*0.5f};
    dl->AddCircleFilled(out_sock, 5.f, IM_COL32(120,200,120,255));

    
    const float TNW = 180.f, TNH = 160.f;
    const float PREV = 96.f; 
    ImVec2 tex_pos = {orig.x + canvas_w - TNW - 40.f, orig.y + canvas_h*0.5f - TNH*0.5f};
    ImU32 tex_hdr  = IM_COL32(55,70,110,255);
    ImU32 tex_body = IM_COL32(35,40,60,255);
    ImU32 tex_bdr  = IM_COL32(80,100,180,200);

    dl->AddRectFilled(tex_pos, {tex_pos.x+TNW, tex_pos.y+26}, tex_hdr, ROUNDING, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled({tex_pos.x, tex_pos.y+26}, {tex_pos.x+TNW, tex_pos.y+TNH}, tex_body, ROUNDING, ImDrawFlags_RoundCornersBottom);
    dl->AddRect(tex_pos, {tex_pos.x+TNW, tex_pos.y+TNH}, tex_bdr, ROUNDING);
    dl->AddText({tex_pos.x+8, tex_pos.y+6}, IM_COL32(160,180,255,255), "Texture");

    
    ImVec2 in_sock = {tex_pos.x, tex_pos.y+TNH*0.5f};
    dl->AddCircleFilled(in_sock, 5.f, IM_COL32(100,140,220,255));

    
    ImVec2 cp1 = {out_sock.x + 60.f, out_sock.y};
    ImVec2 cp2 = {in_sock.x  - 60.f, in_sock.y};
    dl->AddBezierCubic(out_sock, cp1, cp2, in_sock, IM_COL32(120,180,120,180), 2.f);

    
    ImVec2 prev_tl = {tex_pos.x + (TNW-PREV)*0.5f, tex_pos.y + 32.f};
    std::string cur_tex = (sm.tex_sel >= 0 && sm.tex_sel < (int)sm.tex_candidates.size())
                          ? sm.tex_candidates[sm.tex_sel] : "";
    if (!cur_tex.empty()) {
        
        unsigned int tid = 0;
        for (auto& e : state.all_tex_entries)
            if (e.first == cur_tex) { tid = (unsigned int)ImGui::GetID(e.second.c_str()); break; }
        
        
        dl->AddRectFilled(prev_tl, {prev_tl.x+PREV, prev_tl.y+PREV}, IM_COL32(50,50,80,255));
        dl->AddRect(prev_tl, {prev_tl.x+PREV, prev_tl.y+PREV}, IM_COL32(80,100,180,200));
        dl->AddText({prev_tl.x+4, prev_tl.y+PREV*0.5f-7.f}, IM_COL32(160,170,220,255), cur_tex.c_str());
    } else {
        dl->AddRectFilled(prev_tl, {prev_tl.x+PREV, prev_tl.y+PREV}, IM_COL32(40,40,40,200));
        dl->AddText({prev_tl.x+8, prev_tl.y+PREV*0.5f-7.f}, IM_COL32(100,100,100,255), "(no texture)");
    }
    
    dl->AddText({tex_pos.x+8, prev_tl.y+PREV+4.f},
        IM_COL32(160,170,220,200), cur_tex.empty() ? "" : cur_tex.c_str());

    ImGui::EndChild();

    
    ImGui::SameLine();
    ImGui::BeginChild("##texbrowser", {browser_w, canvas_h}, true);

    ImGui::TextColored({0.7f,0.85f,1.f,1.f}, "Textures (%d)", (int)state.all_tex_entries.size());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##texfilter", state.tex_filter, sizeof(state.tex_filter));
    ImGui::Separator();

    std::string filter_lo = state.tex_filter;
    std::transform(filter_lo.begin(), filter_lo.end(), filter_lo.begin(), ::tolower);

    const float THUMB = 48.f;
    for (auto& [stem, path] : state.all_tex_entries) {
        
        if (!filter_lo.empty()) {
            std::string sl = stem;
            std::transform(sl.begin(), sl.end(), sl.begin(), ::tolower);
            if (sl.find(filter_lo) == std::string::npos) continue;
        }

        
        extern unsigned int load_texture(const std::string&);
        unsigned int tid = load_texture(path);

        bool is_current = (stem == cur_tex);
        if (is_current) ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.2f,0.3f,0.5f,0.6f});

        ImGui::PushID(stem.c_str());
        ImGui::BeginGroup();

        if (tid)
            ImGui::Image((ImTextureID)(uintptr_t)tid, {THUMB, THUMB});
        else {
            ImGui::Dummy({THUMB, THUMB});
            ImDrawList* bdl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetItemRectMin();
            bdl->AddRectFilled(p, {p.x+THUMB, p.y+THUMB}, IM_COL32(40,40,40,200));
            bdl->AddText({p.x+4, p.y+THUMB*0.5f-7.f}, IM_COL32(80,80,80,255), "?");
        }

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(stem.c_str());
        if (is_current) ImGui::TextColored({0.4f,0.8f,0.4f,1.f}, "active");
        if (ImGui::SmallButton("Assign")) {
            if (cb.on_tex_assign) cb.on_tex_assign(state.sel_submesh, stem);
            sm.tex_candidates = {stem};
            sm.tex_sel = 0;
            sm.has_tex = tid != 0;
        }
        ImGui::EndGroup();
        ImGui::EndGroup();

        if (is_current) ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::Separator();
    }

    ImGui::EndChild();
    ImGui::End();
}
