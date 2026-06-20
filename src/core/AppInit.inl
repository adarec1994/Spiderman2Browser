bool App::init(int w, int h, const char* title) {
    g_app=this; m_w=w; m_h=h;
    if (!glfwInit()) return false;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GL_TRUE);
#endif
    m_window = glfwCreateWindow(w,h,title,nullptr,nullptr);
    if (!m_window) { glfwTerminate(); return false; }
    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return false;

    glfwSetKeyCallback            (m_window,cb_key);
    glfwSetMouseButtonCallback    (m_window,cb_mouse_btn);
    glfwSetCursorPosCallback      (m_window,cb_cursor);
    glfwSetScrollCallback         (m_window,cb_scroll);
    glfwSetFramebufferSizeCallback(m_window,cb_resize);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding=4.f;
    ImGui_ImplGlfw_InitForOpenGL(m_window,true);
    ImGui_ImplOpenGL3_Init("#version 330");

    m_renderer.init();
    setup_callbacks();

    
    
    
    std::string saved_xiso, saved_folder;
    UI::load_config(saved_xiso, saved_folder);

    
    
    
    std::string picked = !saved_xiso.empty() ? saved_xiso : saved_folder;
    if (!picked.empty() && fs::exists(picked)) {
        std::string root = resolve_source(picked);
        if (vfs::mounted()) m_ui_state.xiso_path = picked;
        if (!root.empty() && vfs::exists(root)) {
            m_ui_state.folder = root;
            scan_folder(root);
        } else {
            m_ui_state.show_splash = true;
        }
    } else {
        m_ui_state.show_splash = true;
    }
    return true;
}

void App::setup_callbacks() {
    m_ui_cb.on_scan_folder  = [this](const std::string& f){
        vfs::unmount();                       
        m_ui_state.folder = f;
        
        m_ui_state.xiso_path.clear();
        UI::save_config("", f);
        m_ui_state.show_splash = false;
        scan_folder(f);
    };
    m_ui_cb.on_select_xiso  = [this](const std::string& xiso){
        m_ui_state.xiso_path = xiso;
        
        std::string root = resolve_source(xiso);
        m_ui_state.folder = root;
        UI::save_config(xiso, root);
        m_ui_state.show_splash = false;
        if (!root.empty() && vfs::exists(root)) scan_folder(root);
    };
    m_ui_cb.on_select_file  = [this](int i){ m_ui_state.selected=i; load_file(i); };
    m_ui_cb.on_reset_camera = [this](){ m_cam.reset(); m_model_rot_y=0.f; m_renderer.model_rot_y=0.f; };
    m_ui_cb.on_select_anim  = [this](int i){ select_animation(i); m_ui_state.anim_sel=i; };
    m_ui_cb.on_extract_anim = [this](int i){ extract_animation(i); };
    m_ui_cb.on_extract_all_anims = [this](){ extract_all_animations(); };
    m_ui_cb.on_extract_model = [this](int i){ extract_model(i); };
    m_ui_cb.on_export_model = [this](int i, const std::string& fmt, const std::string& path){
        export_model(i, fmt, path);
    };
    m_ui_cb.on_tex_assign = [this](int smi, const std::string& stem) {
        if (!m_gpu_model || smi < 0 || smi >= (int)m_gpu_model->meshes.size()) return;
        std::string dir = fs::path(m_ui_state.files[m_ui_state.selected]).parent_path().string();
        unsigned int tid = find_texture(stem, dir);
        m_gpu_model->meshes[smi].tex_id = tid;
        if (smi < (int)m_ui_state.submeshes.size())
            m_ui_state.submeshes[smi].has_tex = tid != 0;
    };
    m_ui_cb.on_play_anim    = [this](){
        if (m_anim_sel<0) return;
        if (!m_anim_play && m_anim_sel >= 0 && m_anim_sel < (int)m_anim_clips.size()) {
            const AnimClip& clip = m_anim_clips[m_anim_sel];
            float dur = clip.duration > 0 ? clip.duration
                      : (clip.fps > 0 && clip.frame_count > 1
                         ? (float)(clip.frame_count - 1) / clip.fps : 1.f);
            if (m_anim_time >= dur) m_anim_time = 0.f;
        }
        m_anim_play = !m_anim_play;
        m_ui_state.anim_playing = m_anim_play;
        m_ui_state.anim_time = m_anim_time;
        m_last_frame = glfwGetTime();
    };
    m_ui_cb.on_load_world_file = [this](int i){
        if (i >= 0 && i < (int)m_ui_state.world_files.size())
            load_world(m_ui_state.world_files[i]);
    };
    
    
    m_ui_cb.on_load_all_worlds = [this](){ m_pending_load_all = true; };
}
