#include "UI.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int UI::PANEL_W = 300;

static const char* CFG_PATH = "xbx_viewer.cfg";

void UI::save_folder(const std::string& f) {
    if (std::ofstream o(CFG_PATH); o) o << f;
}

std::string UI::load_folder() {
    std::ifstream f(CFG_PATH);
    std::string s; std::getline(f, s); return s;
}

void UI::draw(UIState& state, UICallbacks& cb,
              bool& wireframe, bool& show_grid, bool& show_skel, int) {

    float display_h = ImGui::GetIO().DisplaySize.y;
    float display_w = ImGui::GetIO().DisplaySize.x;
    float line      = ImGui::GetTextLineHeightWithSpacing();
    float& style_y  = ImGui::GetStyle().ItemSpacing.y;

    // ── Pinned bottom panel ───────────────────────────────
    float bot_h = line * 4.f + ImGui::GetStyle().FramePadding.y * 8 + style_y * 5;
    ImGui::SetNextWindowPos({0, display_h - bot_h});
    ImGui::SetNextWindowSize({(float)PANEL_W, bot_h});
    ImGui::Begin("##opts", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar);
    ImGui::Checkbox("Wireframe", &wireframe);
    ImGui::Checkbox("Grid",      &show_grid);
    ImGui::Checkbox("Skeleton",  &show_skel);
    if (ImGui::Button("Reset Camera")) cb.on_reset_camera();
    ImGui::End();

    // ── Scrollable top panel (list + status) ──────────────
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)PANEL_W, display_h - bot_h});
    ImGui::Begin("##panel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── Header ────────────────────────────────────────────
    ImGui::TextColored({0.6f,0.8f,1.f,1.f}, "XBX Model Viewer");
    ImGui::Separator();

    // ── Folder input ──────────────────────────────────────
    static char fbuf[1024] = {};
    if (state.folder != std::string(fbuf))
        strncpy(fbuf, state.folder.c_str(), 1023);

    float bw = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 2 + 4;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText("##fld", fbuf, 1024)) state.folder = fbuf;

    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = state.folder.empty() ? "." : state.folder;
        cfg.flags = ImGuiFileDialogFlags_Modal;
        ImGuiFileDialog::Instance()->OpenDialog("FD", "Choose Folder", nullptr, cfg);
    }

    // File dialog
    float dw = std::min(700.f, display_w * 0.8f);
    float dh = std::min(450.f, display_h * 0.7f);
    ImGui::SetNextWindowPos({(display_w-dw)*0.5f, (display_h-dh)*0.5f}, ImGuiCond_Always);
    if (ImGuiFileDialog::Instance()->Display("FD", ImGuiWindowFlags_NoCollapse, {dw,dh}, {dw,dh})) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            state.folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            strncpy(fbuf, state.folder.c_str(), 1023);
            save_folder(state.folder);
            cb.on_scan_folder(state.folder);
        }
        ImGuiFileDialog::Instance()->Close();
    }

    // Scan button + count
    if (ImGui::Button("Scan") && !state.folder.empty()) {
        save_folder(state.folder);
        cb.on_scan_folder(state.folder);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d files", (int)state.files.size());
    ImGui::Separator();

    // ── File list ─────────────────────────────────────────
    ImGuiListClipper clipper;
    clipper.Begin((int)state.files.size());
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            std::string lbl = fs::path(state.files[i]).filename().string()
                              + "##" + std::to_string(i);
            if (ImGui::Selectable(lbl.c_str(), i == state.selected) && i != state.selected)
                cb.on_select_file(i);
        }
    clipper.End();
    ImGui::Separator();

    // ── Status / mesh info ────────────────────────────────
    if (!state.status_msg.empty()) {
        bool err = state.status_msg.rfind("Error",0)==0 || state.status_msg.rfind("Failed",0)==0;
        ImGui::TextColored(err ? ImVec4{1,.3f,.3f,1} : ImVec4{.3f,1,.5f,1},
                           "%s", state.status_msg.c_str());
    }
    if (state.has_model && state.selected >= 0 && state.selected < (int)state.files.size()) {
        ImGui::TextWrapped("%s", fs::path(state.files[state.selected]).filename().string().c_str());
        ImGuiListClipper clip2;
        clip2.Begin((int)state.mesh_info.size());
        while (clip2.Step())
            for (int i = clip2.DisplayStart; i < clip2.DisplayEnd; ++i)
                ImGui::TextDisabled("  %s", state.mesh_info[i].c_str());
        clip2.End();
    }


    ImGui::End();
}