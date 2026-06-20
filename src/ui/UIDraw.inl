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
                    if (ImGui::MenuItem("Export to GLB")) {
                        open_model_export_dialog(idx, "glb", state);
                    }
                    if (ImGui::MenuItem("Export to FBX")) {
                        open_model_export_dialog(idx, "fbx", state);
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

    float fdw = std::min(760.f, dw * 0.85f);
    float fdh = std::min(500.f, dh * 0.75f);
    ImGui::SetNextWindowPos({(dw - fdw) * 0.5f, (dh - fdh) * 0.5f}, ImGuiCond_Appearing);
    if (ImGuiFileDialog::Instance()->Display("MODEL_EXPORT",
            ImGuiWindowFlags_NoCollapse, {fdw, fdh}, {fdw, fdh})) {
        if (ImGuiFileDialog::Instance()->IsOk() &&
            s_pending_model_export_idx >= 0 &&
            !s_pending_model_export_format.empty()) {
            std::string path = with_extension(ImGuiFileDialog::Instance()->GetFilePathName(),
                                              s_pending_model_export_format);
            if (cb.on_export_model)
                cb.on_export_model(s_pending_model_export_idx, s_pending_model_export_format, path);
        }
        s_pending_model_export_idx = -1;
        s_pending_model_export_format.clear();
        ImGuiFileDialog::Instance()->Close();
    }

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
