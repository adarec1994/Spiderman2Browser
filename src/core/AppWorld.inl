void App::scan_folder(const std::string& folder) {
    m_ui_state.files.clear();
    m_ui_state.selected=-1;
    m_ui_state.folder=folder;
    for (auto& fp : vfs::walk_files(folder)) {
        std::string ext = fs::path(fp).extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),::tolower);
        if (ext==".xbx") m_ui_state.files.push_back(fp);
    }
    std::sort(m_ui_state.files.begin(),m_ui_state.files.end());
    m_ui_state.status_msg = std::to_string(m_ui_state.files.size())+" files found";

    
    build_tex_registry(folder);
    get_registry_entries(m_ui_state.all_tex_entries);

    
    load_animations(folder);

    
    m_ui_state.world_files.clear();
    {
        for (auto& fp : vfs::walk_files(folder)) {
            fs::path p = fp;
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".dat") continue;
            std::string stem = p.stem().string();
            
            
            
            {
                
                if (stem.find('_') != std::string::npos) continue;
                size_t i = 0;
                
                while (i < stem.size() && std::isupper((unsigned char)stem[i])) ++i;
                if (i < 1 || i > 3) continue;
                size_t alpha_end = i;
                
                while (i < stem.size() && std::isdigit((unsigned char)stem[i])) ++i;
                if (i - alpha_end < 2 || i - alpha_end > 3) continue;
                
                while (i < stem.size() && std::isupper((unsigned char)stem[i])) ++i;
                if (i != stem.size()) continue; 
            }
            m_ui_state.world_files.push_back(fp);
        }
        std::sort(m_ui_state.world_files.begin(), m_ui_state.world_files.end());
    }


    
    m_xbx_registry.clear();
    {
        for (auto& fp : vfs::walk_files(folder)) {
            std::string fnl = fs::path(fp).filename().string();
            std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
            if (fs::path(fnl).extension() != ".xbx") continue;
            std::string stem = fs::path(fnl).stem().string();
            if (m_xbx_registry.find(stem) == m_xbx_registry.end())
                m_xbx_registry[stem] = fp;
        }

        
        
        m_xbx_base_index.clear();
        for (auto& [stem, path] : m_xbx_registry) {
            
            std::string base = stem;
            if (base.size() > 9) {
                std::string tail = base.substr(base.size() - 9);
                if (tail[0] == '_' && std::all_of(tail.begin()+1, tail.end(), ::isdigit))
                    base = base.substr(0, base.size() - 9);
            }
            
            if (base == stem) {
                while (!base.empty() && std::isdigit((unsigned char)base.back()))
                    base.pop_back();
            }
            if (base != stem && base.size() >= 3) {
                
                auto it = m_xbx_base_index.find(base);
                if (it == m_xbx_base_index.end() || stem < it->second)
                    m_xbx_base_index[base] = stem;
            }
        }

        
        
        m_xbx_suffix_index.clear();
        for (auto& [stem, path] : m_xbx_registry) {
            std::string s = stem;
            while (!s.empty()) {
                
                std::string base = s;
                while (!base.empty() && (std::isdigit((unsigned char)base.back()) || base.back() == '_'))
                    base.pop_back();
                if (base.size() >= 4 && m_xbx_suffix_index.find(base) == m_xbx_suffix_index.end())
                    m_xbx_suffix_index[base] = stem;
                auto us = s.find('_');
                if (us == std::string::npos) break;
                s = s.substr(us + 1);
            }
        }

    }
}



void App::clear_world() {
    
    
    m_instanced_world.release();
    for (auto& [k, gm] : m_world_gpu_cache) { if (gm) { gm->release(); delete gm; } }
    m_world_gpu_cache.clear();
    m_world_draws.clear();
    m_world_bb_min = glm::vec3( 1e9f);
    m_world_bb_max = glm::vec3(-1e9f);
    m_world_mode = false;
    m_cam.fly    = false;    
    m_fly_look   = false;
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    m_ui_state.world_mode            = false;
    m_ui_state.world_instance_count  = 0;
    m_ui_state.world_prop_count      = 0;
    m_ui_state.world_dat_path        = {};
    m_ui_state.world_load_progress   = -1.f;
    m_ui_state.world_load_status     = {};
}

GPUModel* App::world_get_or_load_model(const std::string& asset_name) {
    if (asset_name.empty()) return nullptr;

    
    
    
    
    
    
    
    {
        bool any_clean = false;
        size_t i = 0, n = asset_name.size();
        while (i < n && !any_clean) {
            size_t j = i;
            while (j < n && asset_name[j] != '_') ++j;
            int len = (int)(j - i), letters = 0, up = 0, lo = 0, other = 0;
            for (size_t k = i; k < j; ++k) {
                unsigned char c = (unsigned char)asset_name[k];
                if      (std::isupper(c)) { ++up; ++letters; }
                else if (std::islower(c)) { ++lo; ++letters; }
                else if (!std::isdigit(c)) ++other;
            }
            if (len >= 3 && letters >= 1 && other == 0 && !(up && lo)) any_clean = true;
            i = j + 1;
        }
        if (!any_clean) return nullptr;
    }

    std::string key = asset_name;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto it = m_world_gpu_cache.find(key);
    if (it != m_world_gpu_cache.end()) return it->second;

    auto try_load = [&](const std::string& stem) -> GPUModel* {
        auto rit = m_xbx_registry.find(stem);
        if (rit == m_xbx_registry.end()) return nullptr;
        XBXModel* xm = parse_xbx(rit->second);
        if (!xm) {
            return nullptr;
        }
        GPUModel* gm = m_renderer.upload_model(xm);
        delete xm;
        return gm;
    };

    auto try_stem = [&](const std::string& stem) -> GPUModel* {
        if (stem.size() < 3) return nullptr;

        
        if (auto* gm = try_load(stem)) return gm;

        
        
        
        
        
        
        if (stem.size() >= 6) {
            size_t n = stem.size();
            if (std::isdigit((unsigned char)stem[n-1]) && std::isdigit((unsigned char)stem[n-2]) &&
                std::isdigit((unsigned char)stem[n-3])) {
                std::string s3 = stem.substr(0, n - 3);
                if (auto* gm = try_load(s3)) return gm;
            }
        }

        
        std::string base = stem;
        while (!base.empty() && (std::isdigit((unsigned char)base.back()) || base.back() == '_'))
            base.pop_back();
        if (base.empty() || base.size() < 3) return nullptr;

        
        if (base != stem)
            if (auto* gm = try_load(base)) return gm;

        
        {
            auto bi = m_xbx_base_index.find(base);
            if (bi != m_xbx_base_index.end())
                if (auto* gm = try_load(bi->second)) return gm;
        }

        
        
        
        if (base.size() >= 8) {
            
            auto si = m_xbx_suffix_index.find(base);
            if (si != m_xbx_suffix_index.end())
                if (auto* gm = try_load(si->second)) return gm;

            
            
            for (auto& [k, v] : m_xbx_suffix_index) {
                if (k.size() > base.size() && k.rfind(base, 0) == 0)
                    if (auto* gm = try_load(v)) return gm;
            }
        }

        return nullptr;
    };

    
    std::string cur = key;
    int strips = 0;
    while (!cur.empty() && strips <= 4) {
        if (auto* gm = try_stem(cur)) { m_world_gpu_cache[key] = gm; return gm; }
        auto us = cur.find('_');
        if (us == std::string::npos) break;
        cur = cur.substr(us + 1);
        ++strips;
    }

    m_world_gpu_cache[key] = nullptr;
    return nullptr;
}

















void App::build_world_draws(const WorldData& wd) {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s;
    };

    auto acc_bb = [&](const glm::mat4& xf) {
        glm::vec3 p = glm::vec3(xf[3]);
        m_world_bb_min = glm::min(m_world_bb_min, p);
        m_world_bb_max = glm::max(m_world_bb_max, p);
    };

    
    
    
    
    
    
    
    {
        auto pick = [&](const std::string& master, const char* prefer) -> std::string {
            std::string best;
            for (auto& bt : wd.blg_textures) {
                if (bt.master != master) continue;
                if (best.empty()) best = bt.texture;                 
                if (bt.texture.find(prefer) != std::string::npos) return bt.texture; 
            }
            return best;
        };
        std::string wall = pick("s_blg_blgmaster", "sid1");
        std::string ped  = pick("s_blg_pedmaster", "pedbas1");
        std::string trm  = pick("s_blg_trm",       "trm");
        if (!wall.empty()) register_tex_alias("s_blg_blgmaster", wall);
        if (!ped.empty())  register_tex_alias("s_blg_pedmaster", ped);
        if (!trm.empty())  register_tex_alias("s_blg_trm",       trm);
    }

    for (auto& inst : wd.instances) {
        GPUModel* gm = world_get_or_load_model(inst.asset_name);
        
        
        
        
        
        if (!gm && !inst.name.empty() && inst.name != inst.asset_name)
            gm = world_get_or_load_model(inst.name);
        if (!gm) continue;
        m_world_draws.push_back({ gm, inst.transform });
        acc_bb(inst.transform);
    }
    
    
    
    std::vector<unsigned int> blg_pal;
    std::unordered_map<int, unsigned int> blg_slot2tex;
    for (auto& bt : wd.blg_textures) {
        unsigned int t = find_texture_world(bt.texture);
        if (!t) continue;
        blg_pal.push_back(t);
        blg_slot2tex.emplace(bt.slot, t);
    }

    for (auto& prop : wd.props) {
        if (prop.type_idx < 0 || prop.type_idx >= (int)wd.prop_types.size()) continue;
        GPUModel* gm = world_get_or_load_model(wd.prop_types[prop.type_idx]);
        if (!gm) continue;
        float yr = glm::radians(prop.yaw_deg);
        
        
        unsigned int ov = 0;
        if (prop.slot >= 0 && !blg_pal.empty()) {
            auto it = blg_slot2tex.find(prop.slot);
            ov = (it != blg_slot2tex.end()) ? it->second
                                            : blg_pal[(size_t)prop.slot % blg_pal.size()];
        }
        
        
        
        
        
        float fh = gm->bb_ymax - gm->bb_ymin;
        int floors = 1;
        if (prop.height > 0.f && fh > 0.5f) {
            floors = (int)std::lround(prop.height / fh);
            floors = std::max(1, std::min(floors, 60));
        }
        for (int f = 0; f < floors; ++f) {
            glm::mat4 xf = glm::translate(glm::mat4(1.f), glm::vec3(prop.x, prop.y + (float)f * fh, prop.z));
            xf = glm::rotate(xf, yr, glm::vec3(0.f, 1.f, 0.f));
            m_world_draws.push_back({ gm, xf, ov });
            if (f == 0) acc_bb(xf);
        }
    }

}

void App::finalize_world_merge() {
    if (m_world_draws.empty()) return;

    
    
    
    
    std::vector<WorldPlacement> insts;
    insts.reserve(m_world_draws.size());
    for (auto& dc : m_world_draws) insts.push_back({ dc.model, dc.xform, dc.tex_override });

    m_instanced_world.release();
    m_instanced_world = m_renderer.build_instanced_world(insts);

    
    
    
    m_world_draws.clear();
}

void App::recentre_camera_on_world() {
    
    
    glm::vec3 mn = m_world_bb_min, mx = m_world_bb_max;
    if (mn.x > mx.x) return;  
    glm::vec3 centre = (mn + mx) * 0.5f;
    glm::vec3 size   = mx - mn;
    float     extent = glm::length(size);

    
    
    
    
    
    
    
    float height  = glm::clamp(size.y * 1.5f + 40.f, 40.f, 400.f);
    float standoff = glm::clamp(std::max(size.x, size.z) * 0.10f, 60.f, 600.f);
    glm::vec3 start = centre + glm::vec3(0.f, height, standoff);
    m_cam.reset_fly(start);
    
    m_cam.fly_speed = glm::clamp(extent * 0.04f, 20.f, 400.f);

    
    glm::vec3 dir = glm::normalize(centre - start);
    m_cam.yaw   = glm::degrees(atan2f(dir.x, dir.z));
    m_cam.pitch = glm::degrees(asinf(glm::clamp(dir.y, -1.f, 1.f)));
}




void App::load_sector_terrain(const std::string& dat_path) {
    std::string stem = fs::path(dat_path).stem().string();
    std::string key  = stem;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    
    GPUModel* gm = world_get_or_load_model(key);
    if (!gm) gm = world_get_or_load_model(key + "r");
    if (gm)
        m_world_draws.push_back({ gm, glm::mat4(1.f) });
}

void App::load_world(const std::string& dat_path) {
    clear_world();
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; m_gpu_model = nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel;  m_gpu_skel  = nullptr; }
    m_ui_state.has_model = false;
    m_ui_state.submeshes.clear();

    m_ui_state.world_load_progress = 0.f;
    m_ui_state.world_load_status   = "Parsing...";

    WorldData* wd = parse_world(dat_path);
    if (!wd) {
        m_ui_state.world_load_progress = -1.f;
        m_ui_state.status_msg = "Error: failed to parse world file";
        return;
    }

    int total = (int)wd->instances.size() + (int)wd->props.size();
    int done  = 0;

    m_ui_state.world_load_status = "Loading models...";
    for (auto& inst : wd->instances) {
        world_get_or_load_model(inst.asset_name);
        m_ui_state.world_load_progress = total > 0 ? (float)++done / total : 1.f;
    }
    for (auto& prop : wd->props) {
        if (prop.type_idx >= 0 && prop.type_idx < (int)wd->prop_types.size())
            world_get_or_load_model(wd->prop_types[prop.type_idx]);
        m_ui_state.world_load_progress = total > 0 ? (float)++done / total : 1.f;
    }

    build_world_draws(*wd);
    load_sector_terrain(dat_path);
    finalize_world_merge();   
    recentre_camera_on_world();
    m_world_mode                     = true;
    m_ui_state.world_mode            = true;
    m_ui_state.world_instance_count  = (int)wd->instances.size();
    m_ui_state.world_prop_count      = (int)wd->props.size();
    m_ui_state.world_dat_path        = dat_path;
    m_ui_state.world_load_progress   = -1.f;  
    m_ui_state.status_msg            = "World loaded";
    delete wd;
}

void App::pump_loading_frame(const char* label, float frac) {
    glfwPollEvents();
    glfwGetFramebufferSize(m_window, &m_w, &m_h);
    glViewport(0, 0, m_w, m_h);
    glClearColor(0.07f, 0.07f, 0.09f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos({ m_w * 0.5f, m_h * 0.5f }, ImGuiCond_Always, { 0.5f, 0.5f });
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##loading", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored({ 0.4f, 1.f, 0.6f, 1.f }, "Loading all world sectors...");
    ImGui::Dummy({ 360.f, 2.f });
    ImGui::ProgressBar(frac, { 360.f, 0.f });
    if (label && *label) ImGui::TextDisabled("%s", label);
    ImGui::End();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(m_window);
}

void App::load_all_worlds() {
    clear_world();
    if (m_gpu_model) { m_gpu_model->release(); delete m_gpu_model; m_gpu_model = nullptr; }
    if (m_gpu_skel)  { m_gpu_skel->release();  delete m_gpu_skel;  m_gpu_skel  = nullptr; }
    m_ui_state.has_model = false;
    m_ui_state.submeshes.clear();

    int n_files = (int)m_ui_state.world_files.size();
    if (n_files == 0) return;

    int total_inst = 0, total_props = 0;
    for (int fi = 0; fi < n_files; ++fi) {
        m_ui_state.world_load_progress = (float)fi / n_files;
        std::string fname = fs::path(m_ui_state.world_files[fi]).filename().string();
        m_ui_state.world_load_status   = fname;

        
        
        if (fi % 4 == 0)
            pump_loading_frame(fname.c_str(), (float)fi / n_files);

        WorldData* wd = parse_world(m_ui_state.world_files[fi]);
        if (!wd) continue;
        total_inst  += (int)wd->instances.size();
        total_props += (int)wd->props.size();
        build_world_draws(*wd);
        load_sector_terrain(m_ui_state.world_files[fi]);
        delete wd;
    }

    pump_loading_frame("Merging geometry...", 1.f);
    finalize_world_merge();   
    recentre_camera_on_world();
    m_world_mode                     = true;
    m_ui_state.world_mode            = true;
    m_ui_state.world_instance_count  = total_inst;
    m_ui_state.world_prop_count      = total_props;
    m_ui_state.world_dat_path        = "(all)";
    m_ui_state.world_load_progress   = -1.f;
    m_ui_state.status_msg            = "All worlds loaded";
}
