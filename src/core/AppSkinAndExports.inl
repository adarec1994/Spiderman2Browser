void App::compute_skinning() {
    for (int i = 0; i < N_BONES; ++i)
        m_skinning[i] = m_cur_pose[i] * m_inv_bind[i];
}

void App::upload_skinning() {
    compute_skinning();
    m_renderer.set_bone_matrices(m_skinning.data(), N_BONES);
}



void App::load_animations(const std::string& folder) {
    m_anim_clips = scan_animations(folder);
    m_anim_sel   = -1;
    m_anim_play  = false;
    m_anim_time  = 0.f;
    m_anim_bone_map.clear();

    m_ui_state.anim_names.clear();
    m_ui_state.anim_names.reserve(m_anim_clips.size());
    for (auto& c : m_anim_clips)
        m_ui_state.anim_names.push_back(c.name);

}

void App::filter_animations_for_model(const std::string& model_path) {
    if (!is_minion_lizard_model_path(model_path)) return;

    m_anim_clips.erase(
        std::remove_if(m_anim_clips.begin(), m_anim_clips.end(),
            [](const AnimClip& c) {
                std::string name = lower_copy(c.name);
                return name.rfind("lzmn", 0) != 0;
            }),
        m_anim_clips.end());

    m_anim_sel = -1;
    m_anim_play = false;
    m_anim_time = 0.f;
    m_anim_bone_map.clear();
    m_anim_global_ref_set = false;

    m_ui_state.anim_names.clear();
    m_ui_state.anim_names.reserve(m_anim_clips.size());
    for (auto& c : m_anim_clips)
        m_ui_state.anim_names.push_back(c.name);
    m_ui_state.anim_sel = -1;
    m_ui_state.anim_time = 0.f;
    m_ui_state.anim_dur = 0.f;
    m_ui_state.anim_playing = false;
}

void App::extract_animation(int idx) {
    if (idx < 0 || idx >= (int)m_anim_clips.size()) return;

    const AnimClip& clip = m_anim_clips[idx];
    fs::path src = clip.path;
    if (src.empty() || !fs::exists(src)) {
        m_ui_state.status_msg = "Animation source not found";
        return;
    }

    fs::path project_root = fs::current_path();
    std::string cwd_name = project_root.filename().string();
    if (cwd_name.rfind("cmake-build", 0) == 0 && project_root.has_parent_path())
        project_root = project_root.parent_path();

    fs::path pack_name = src.parent_path().filename();
    if (pack_name.empty()) pack_name = "unknown_pack";

    fs::path dst_dir = project_root / "animations" / pack_name;
    fs::path dst = dst_dir / src.filename();

    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    if (ec) {
        m_ui_state.status_msg = "Could not create animations folder";
        return;
    }

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        m_ui_state.status_msg = "Animation extract failed";
        return;
    }

    m_ui_state.status_msg = "Extracted " + src.filename().string() + " to " + dst_dir.string();
}

void App::extract_all_animations() {
    int n = (int)m_anim_clips.size();
    for (int i = 0; i < n; ++i)
        extract_animation(i);
    if (n > 0)
        m_ui_state.status_msg = "Extracted " + std::to_string(n) + " animations";
}


void App::extract_model(int idx) {
    if (idx < 0 || idx >= (int)m_ui_state.files.size()) return;
    fs::path src = m_ui_state.files[idx];
    if (src.empty() || !fs::exists(src)) {
        m_ui_state.status_msg = "Model source not found";
        return;
    }

    fs::path project_root = fs::current_path();
    std::string cwd_name = project_root.filename().string();
    if (cwd_name.rfind("cmake-build", 0) == 0 && project_root.has_parent_path())
        project_root = project_root.parent_path();

    fs::path pack_name = src.parent_path().filename();
    if (pack_name.empty()) pack_name = "unknown_pack";
    fs::path dst_dir = project_root / "extracted_models" / pack_name;

    std::error_code ec;
    fs::create_directories(dst_dir, ec);
    if (ec) {
        m_ui_state.status_msg = "Could not create extracted_models folder";
        return;
    }

    fs::copy_file(src, dst_dir / src.filename(), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        m_ui_state.status_msg = "Model extract failed";
        return;
    }

    int skel = 0;
    auto skeletons = ordered_skeleton_candidates(src);
    if (!skeletons.empty()) {
        fs::path skel_src = skeletons.front();
        std::error_code ec2;
        fs::copy_file(skel_src, dst_dir / skel_src.filename(), fs::copy_options::overwrite_existing, ec2);
        if (!ec2) skel = 1;
    }

    m_ui_state.status_msg = "Extracted " + src.filename().string()
                          + " (+" + std::to_string(skel) + " skel) to " + dst_dir.string();
}

void App::export_model(int idx, const std::string& format, const std::string& output_path) {
    if (idx < 0 || idx >= (int)m_ui_state.files.size()) return;
    std::string fmt = lower_copy(format);
    if (fmt != "glb" && fmt != "fbx") return;
    const std::string path = m_ui_state.files[idx];
    if (!vfs::exists(path)) {
        m_ui_state.status_msg = "Model source not found";
        return;
    }

    m_ui_state.status_msg = "Exporting " + fmt + "...";
    std::unique_ptr<XBXModel> model(parse_xbx(path, true));
    if (!model) {
        m_ui_state.status_msg = "Export failed: model parse failed";
        return;
    }

    std::string skel_path;
    std::unique_ptr<Skeleton> skeleton;
    for (const std::string& candidate : ordered_skeleton_candidates(fs::path(path))) {
        Skeleton* sk = parse_skeleton(candidate, path);
        if (sk) {
            skel_path = candidate;
            skeleton.reset(sk);
            break;
        }
    }

    int bone_count = skeleton ? std::min((int)skeleton->bones.size(), N_BONES) : 0;
    SkeletonAnimMeta meta = load_skeleton_meta(skel_path, bone_count);
    bool minion = is_minion_lizard_model_path(path);
    if (minion)
        meta.quat_effective_scale_cap = 0.0078125f;

    std::vector<AnimClip> clips = scan_animations(fs::path(path).parent_path().string());
    if (minion) {
        clips.erase(
            std::remove_if(clips.begin(), clips.end(),
                [](const AnimClip& c) {
                    std::string name = lower_copy(c.name);
                    return name.rfind("lzmn", 0) != 0;
                }),
            clips.end());
    }

    if (skeleton) {
        for (AnimClip& clip : clips) {
            parse_animation(clip, meta);
            if (!clip.loaded) continue;
            if (!meta.rest_pose.empty()) {
                clip.rest_pose.resize(clip.n_bones);
                for (int ai = 0; ai < clip.n_bones; ++ai) {
                    int si = (ai < (int)clip.bone_indices.size()) ? clip.bone_indices[ai] : ai;
                    clip.rest_pose[ai] = (si >= 0 && si < (int)meta.rest_pose.size())
                                       ? meta.rest_pose[si]
                                       : glm::quat(1, 0, 0, 0);
                }
            }
            clip.frames_decoded = false;
            clip.cached_frames.clear();
            clip.cached_root_orientations.clear();
        }
        clips.erase(std::remove_if(clips.begin(), clips.end(),
            [](const AnimClip& c){ return !c.loaded; }), clips.end());
    } else {
        clips.clear();
    }

    model_export::ExportRequest req;
    req.model = model.get();
    req.skeleton = skeleton.get();
    req.skel_meta = &meta;
    req.animations = &clips;
    req.output_path = output_path;
    req.minion_lizard = minion;

    std::string error;
    bool ok = fmt == "glb" ? model_export::export_glb(req, error)
                           : model_export::export_fbx(req, error);
    if (!ok) {
        m_ui_state.status_msg = error.empty() ? "Export failed" : error;
        return;
    }
    m_ui_state.status_msg = "Exported " + fs::path(output_path).filename().string();
}
