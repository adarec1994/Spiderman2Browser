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
