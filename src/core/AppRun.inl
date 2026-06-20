void App::load_file(int idx) {
    if (idx<0||idx>=(int)m_ui_state.files.size()) return;
    const std::string& path = m_ui_state.files[idx];

    if (m_world_mode) clear_world();
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; m_gpu_model=nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel;  m_gpu_skel=nullptr; }
    m_skeleton.reset();
    m_has_bones=false;
    m_ui_state.has_model=false;
    m_ui_state.submeshes.clear();
    m_ui_state.status_msg="Loading...";
    m_sel_bone=-1; m_renderer.sel_bone=-1;
    m_model_rot_y=0.f; m_renderer.model_rot_y=0.f;
    m_anim_sel=-1; m_anim_play=false; m_anim_time=0.f;
    m_ui_state.anim_sel=-1; m_ui_state.anim_playing=false;
    m_full_rest_pose.clear();
    m_anim_global_ref_set = false;  
    m_minion_lizard_model = is_minion_lizard_model_path(path);

    XBXModel* model = parse_xbx(path,  true);
    if (!model) { m_ui_state.status_msg="Error: Not XBXM or no geometry"; return; }

    m_gpu_model = m_renderer.upload_model(model);
    m_ui_state.has_model=true;
    m_ui_state.status_msg="Loaded";

    
    load_animations(fs::path(path).parent_path().string());
    filter_animations_for_model(path);

    m_cached_raw.clear();
    m_cached_raw.resize(model->submeshes.size());
    m_ui_state.submeshes.clear();
    m_ui_state.sel_submesh = -1;
    m_renderer.sel_submesh = -1;
    m_ui_state.preview_tex_id = 0;
    m_ui_state.preview_tex_name.clear();
    for (int i=0;i<(int)model->submeshes.size();++i) {
        auto& sm = model->submeshes[i];
        m_cached_raw[i].raw       = sm.raw_indices;
        m_cached_raw[i].vc        = (uint32_t)sm.positions.size();
        m_cached_raw[i].positions = sm.positions;

        
        int default_sel = 0; 
        if (sm.prim_type == 5) default_sel = 1; 
        if (sm.prim_type == 8) default_sel = 2; 
        if (sm.from_pushbuffer) default_sel = 0; 

        const char* method_label = "TStrip";
        if (default_sel == 1) method_label = "TList";
        if (default_sel == 2) method_label = "QuadList";

        SubmeshInfo si;
        si.mat_name      = sm.mat_name;
        si.shader_type   = sm.shader_type;
        si.tex_candidates = sm.tex_candidates;
        si.tex_sel       = 0;
        si.tri_count     = (int)sm.indices.size()/3;
        si.prim_raw      = sm.prim_type;
        si.prim_method   = method_label;
        si.has_tex       = m_gpu_model->meshes[i].tex_id != 0;
        si.method_sel    = default_sel;
        m_ui_state.submeshes.push_back(si);
    }

    
    if ((int)model->bind_pose.size() >= N_BONES) {
        for (int i=0;i<N_BONES;++i) {
            m_bind_pose[i] = model->bind_pose[i];
            m_inv_bind[i]  = glm::inverse(m_bind_pose[i]);
            m_cur_pose[i]  = m_bind_pose[i];
        }
        for (int i=0;i<N_BONES;++i) m_skinning[i] = glm::mat4(1.f);
        m_has_bones=true;
        m_renderer.set_bone_matrices(m_skinning.data(), N_BONES);
    }

    
    
    
    std::string skel_path;
    for (const std::string& candidate : ordered_skeleton_candidates(fs::path(path))) {
        Skeleton* sk = parse_skeleton(candidate, path);
        if (sk) {
            skel_path = candidate;
            m_skeleton.reset(sk);
            m_gpu_skel = m_renderer.upload_skeleton(sk);
            break;
        }
    }

    
    
    
    int bone_count = m_skeleton ? std::min((int)m_skeleton->bones.size(), N_BONES) : 0;
    m_skel_meta = load_skeleton_meta(skel_path, bone_count);
    if (m_minion_lizard_model)
        m_skel_meta.quat_effective_scale_cap = 0.0078125f;
    m_full_rest_pose = m_skel_meta.rest_pose;

    delete model;
    m_cam.reset();
}



void App::run() {
    m_last_frame = glfwGetTime();
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();
        glfwGetFramebufferSize(m_window,&m_w,&m_h);

        
        
        if (m_pending_load_all) {
            m_pending_load_all = false;
            load_all_worlds();
            m_last_frame = glfwGetTime();
        }
        double now = glfwGetTime();

        
        if (m_world_mode && m_cam.fly && !ImGui::GetIO().WantCaptureKeyboard) {
            unsigned keys = 0;
            if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) keys |= 1;
            if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) keys |= 2;
            if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) keys |= 4;
            if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) keys |= 8;
            if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) keys |= 16;
            if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) keys |= 32;
            if (keys) {
                float dt = (float)(now - m_last_frame);
                if (dt > 0.1f) dt = 0.1f;
                m_cam.fly_move(dt, keys);
            }
        }

        tick_animation(now);

        glViewport(0,0,m_w,m_h);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        m_renderer.sel_submesh = m_ui_state.sel_submesh;
        m_ui.draw(m_ui_state,m_ui_cb,
                  m_renderer.wireframe,m_renderer.show_grid,m_renderer.show_skel,
                  m_renderer.show_uv,m_h);

        
        if (m_rot_mode) {
            ImGui::SetNextWindowPos({(float)vp_x()+10,10});
            ImGui::SetNextWindowBgAlpha(0.65f);
            ImGui::Begin("##rot",nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
                ImGuiWindowFlags_NoMove|ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::TextColored({1.f,0.8f,0.2f,1.f},"ROTATE  R");
            if (m_sel_bone>=0&&m_skeleton)
                ImGui::Text("Bone: %s",m_skeleton->bones[m_sel_bone].name.c_str());
            else ImGui::Text("Model");
            std::string ax = m_rot_axis.x>0.5f?"X":m_rot_axis.y>0.5f?"Y":"Z";
            ImGui::Text("Axis: %s  Angle: %.1f°",ax.c_str(),glm::degrees(m_rot_accum));
            ImGui::TextDisabled("X/Y/Z constrain | LMB/Enter confirm | RMB/Esc cancel");
            ImGui::End();
        }

        
        if (m_world_mode && m_cam.fly) {
            ImGui::SetNextWindowPos({(float)vp_x()+10, 10});
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::Begin("##flyhud", nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
                ImGuiWindowFlags_NoMove|ImGuiWindowFlags_AlwaysAutoResize|
                ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::TextColored({0.4f,1.f,0.6f,1.f}, "FLY CAM");
            ImGui::TextDisabled("WASD  move    Q/E  down/up");
            ImGui::TextDisabled("RMB   look    scroll  speed");
            ImGui::TextDisabled("Speed: %.0f u/s", m_cam.fly_speed);
            if (m_fly_look)
                ImGui::TextColored({1.f,0.8f,0.3f,1.f}, "LOOKING  (release RMB)");
            ImGui::End();
        }

        ImGui::Render();

        int w=vp_w(m_w);
        if (m_world_mode && !m_instanced_world.models.empty()) {
            
            
            m_renderer.draw_instanced_world(m_cam, vp_x(), w, m_h, m_instanced_world);
        } else if (m_world_mode && !m_world_draws.empty()) {
            
            std::vector<std::pair<GPUModel*, glm::mat4>> draws;
            draws.reserve(m_world_draws.size());
            for (auto& dc : m_world_draws) draws.push_back(std::make_pair(dc.model, dc.xform));
            m_renderer.draw_world_instances(m_cam, vp_x(), w, m_h, draws);
        } else {
            m_renderer.draw_scene(m_cam,vp_x(),w,m_h,m_gpu_model,m_gpu_skel);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(m_window);
    }
}

void App::shutdown() {
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel; }
    m_instanced_world.release();
    for (auto& [k, gm] : m_world_gpu_cache) { if (gm) { gm->release(); delete gm; } }
    m_world_gpu_cache.clear();
    m_renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(m_window);
    glfwTerminate();
}
