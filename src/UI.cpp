#include "UI.h"
#include <imgui.h>
#include <ImGuiFileDialog.h>
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace fs = std::filesystem;

int UI::PANEL_W = 300;
static constexpr int ANIM_W = 220;

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

    float dh   = ImGui::GetIO().DisplaySize.y;
    float dw   = ImGui::GetIO().DisplaySize.x;
    float line = ImGui::GetTextLineHeightWithSpacing();
    float sy   = ImGui::GetStyle().ItemSpacing.y;

    // ── Pinned bottom options panel (left side) ───────────────────────────────
    float bot_h = line * 4.f + ImGui::GetStyle().FramePadding.y * 8 + sy * 5;
    ImGui::SetNextWindowPos({0, dh - bot_h});
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

    // ── Left panel: file list ─────────────────────────────────────────────────
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)PANEL_W, dh - bot_h});
    ImGui::Begin("##panel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored({0.6f,0.8f,1.f,1.f}, "XBX Model Viewer");
    ImGui::Separator();

    // Folder input
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

    float fdw = std::min(700.f, dw * 0.8f);
    float fdh = std::min(450.f, dh * 0.7f);
    ImGui::SetNextWindowPos({(dw-fdw)*0.5f,(dh-fdh)*0.5f}, ImGuiCond_Always);
    if (ImGuiFileDialog::Instance()->Display("FD", ImGuiWindowFlags_NoCollapse, {fdw,fdh},{fdw,fdh})) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            state.folder = ImGuiFileDialog::Instance()->GetCurrentPath();
            strncpy(fbuf, state.folder.c_str(), 1023);
            save_folder(state.folder);
            cb.on_scan_folder(state.folder);
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGui::Button("Scan") && !state.folder.empty()) {
        save_folder(state.folder);
        cb.on_scan_folder(state.folder);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d files", (int)state.files.size());
    ImGui::Separator();

    // File list
    ImGuiListClipper clipper;
    clipper.Begin((int)state.files.size());
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            std::string lbl = fs::path(state.files[i]).filename().string()
                              + "##f" + std::to_string(i);
            if (ImGui::Selectable(lbl.c_str(), i == state.selected) && i != state.selected)
                cb.on_select_file(i);
        }
    clipper.End();
    ImGui::Separator();

    // Status
    if (!state.status_msg.empty()) {
        bool err = state.status_msg.rfind("Error",0)==0 || state.status_msg.rfind("Failed",0)==0;
        ImGui::TextColored(err ? ImVec4{1,.3f,.3f,1} : ImVec4{.3f,1,.5f,1},
                           "%s", state.status_msg.c_str());
    }

    ImGui::End();

    // ── Submesh panel — pinned below file list, above bottom bar ─────────────
    if (state.has_model && !state.submeshes.empty()) {
        float sm_h = dh - bot_h - (dh * 0.50f);
        if (sm_h > 60.f) {
            ImGui::SetNextWindowPos({0, dh - bot_h - sm_h});
            ImGui::SetNextWindowSize({(float)PANEL_W, sm_h});
            ImGui::Begin("##submesh", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoScrollbar);

            ImGui::TextColored({0.6f,0.8f,1.f,1.f}, "Submeshes");
            ImGui::Separator();

            // List of submeshes (scrollable, fixed height)
            float list_portion = (state.sel_submesh >= 0) ? sm_h * 0.5f : sm_h - 30.f;
            ImGui::BeginChild("##smnames", ImVec2(0, list_portion), false);
            static const char* prim_opts[] = { "TStrip", "TList", "QuadList", "TFan" };
            for (int i = 0; i < (int)state.submeshes.size(); ++i) {
                auto& sm = state.submeshes[i];
                ImGui::PushID(i);
                bool sel = (state.sel_submesh == i);
                ImGui::TextColored(sel ? ImVec4{0.3f,1.f,0.4f,1.f} : ImVec4{0.8f,0.8f,0.8f,1.f},
                    "SM%d", i);
                ImGui::SameLine();
                if (ImGui::Selectable(sm.mat_name.c_str(), sel))
                    state.sel_submesh = i;
                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::End();
        }
    }

    // ── Face type overlay — top-right of viewport ─────────────────────────────
    if (state.has_model && state.sel_submesh >= 0 &&
        state.sel_submesh < (int)state.submeshes.size()) {
        auto& sm = state.submeshes[state.sel_submesh];
        static const char* prim_opts[] = { "TStrip", "TList", "QuadList", "TFan" };
        const float pad = 8.f;
        const float w   = 220.f;
        const float h   = ImGui::GetTextLineHeightWithSpacing() * 3.f + pad * 2.f;
        ImGui::SetNextWindowPos({dw - ANIM_W - w - pad, pad});
        ImGui::SetNextWindowSize({w, h});
        ImGui::SetNextWindowBgAlpha(0.75f);
        ImGui::Begin("##facetype_ov", nullptr,
            ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::TextDisabled("SM%d  %s  prim=%u",
            state.sel_submesh, sm.mat_name.c_str(), sm.prim_raw);
        ImGui::TextDisabled("%d tris | %s%s",
            sm.tri_count, sm.prim_method, sm.has_tex ? "" : " | no tex");
        ImGui::SetNextItemWidth(w - pad * 2.f);
        if (ImGui::Combo("##ftype", &sm.method_sel, prim_opts, 4))
            if (cb.on_prim_override) cb.on_prim_override(state.sel_submesh, sm.method_sel);
        ImGui::End();
    }

    // ── Right panel: animation list ───────────────────────────────────────────
    ImGui::SetNextWindowPos({dw - ANIM_W, 0});
    ImGui::SetNextWindowSize({(float)ANIM_W, dh});
    ImGui::Begin("##anim", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored({1.f,0.8f,0.4f,1.f}, "Animations");
    ImGui::TextDisabled("%d clips", (int)state.anim_names.size());
    ImGui::Separator();

    // Play controls
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

        // Progress bar
        float prog = (state.anim_dur > 0) ? (state.anim_time / state.anim_dur) : 0.f;
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.2f / %.2fs", state.anim_time, state.anim_dur);
        ImGui::ProgressBar(prog, {-1, 0}, overlay);
        ImGui::Separator();
    } else {
        ImGui::TextDisabled("(none selected)");
        ImGui::TextDisabled("Space = play/stop");
        ImGui::Separator();
    }

    // Clip list
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
        }
    aclip.End();
    ImGui::EndChild();

    ImGui::End();
}