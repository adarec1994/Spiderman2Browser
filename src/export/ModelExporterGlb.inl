bool export_glb(const ExportRequest& req, std::string& error) {
    error.clear();
    if (!req.model || req.output_path.empty()) {
        error = "No model or output path";
        return false;
    }
    fs::path out = req.output_path;
    std::error_code ec;
    if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
    GlbData glb;
    std::vector<int> mat_texture;
    std::vector<TextureRef> textures = export_material_textures(req, mat_texture, error);
    if (!error.empty()) return false;
    int bone_count = 0;
    if (req.skeleton && req.model->bind_pose.size() > 0)
        bone_count = std::min({(int)req.skeleton->bones.size(), (int)req.model->bind_pose.size(), 60});
    std::vector<glm::mat4> local_bind = local_bind_matrices(req, bone_count);
    std::vector<PrimitiveDef> prims;
    prims.reserve(req.model->submeshes.size());
    for (int si = 0; si < (int)req.model->submeshes.size(); ++si) {
        const auto& sm = req.model->submeshes[si];
        if (sm.positions.empty() || sm.indices.empty()) continue;
        PrimitiveDef p;
        std::vector<float> pos;
        pos.reserve(sm.positions.size() * 3u);
        for (auto& v : sm.positions) {
            pos.push_back(v.x);
            pos.push_back(v.y);
            pos.push_back(v.z);
        }
        p.pos = add_float_array(glb, pos, 34962, sm.positions.size(), "VEC3", min3(sm.positions), max3(sm.positions));
        if (sm.uvs.size() == sm.positions.size()) {
            std::vector<float> uv;
            uv.reserve(sm.uvs.size() * 2u);
            for (auto& v : sm.uvs) {
                uv.push_back(v.x);
                uv.push_back(v.y);
            }
            p.uv = add_float_array(glb, uv, 34962, sm.uvs.size(), "VEC2");
        }
        if (bone_count > 0 && sm.bone_indices.size() == sm.positions.size() && sm.bone_weights.size() == sm.positions.size()) {
            std::vector<uint16_t> joints;
            std::vector<float> weights;
            joints.reserve(sm.positions.size() * 4u);
            weights.reserve(sm.positions.size() * 4u);
            for (size_t i = 0; i < sm.positions.size(); ++i) {
                glm::ivec4 ji = sm.bone_indices[i];
                glm::vec4 jw = sm.bone_weights[i];
                joints.push_back((uint16_t)std::clamp(ji.x, 0, bone_count - 1));
                joints.push_back((uint16_t)std::clamp(ji.y, 0, bone_count - 1));
                joints.push_back((uint16_t)std::clamp(ji.z, 0, bone_count - 1));
                joints.push_back((uint16_t)std::clamp(ji.w, 0, bone_count - 1));
                weights.push_back(jw.x);
                weights.push_back(jw.y);
                weights.push_back(jw.z);
                weights.push_back(jw.w);
            }
            p.joints = add_u16_array(glb, joints, 34962, sm.positions.size(), "VEC4");
            p.weights = add_float_array(glb, weights, 34962, sm.positions.size(), "VEC4");
        }
        p.indices = add_u32_array(glb, sm.indices, 34963, sm.indices.size(), "SCALAR");
        p.material = si;
        prims.push_back(p);
    }
    int inv_bind_accessor = -1;
    if (bone_count > 0) {
        std::vector<float> ibm;
        ibm.reserve((size_t)bone_count * 16u);
        for (int i = 0; i < bone_count; ++i) {
            glm::mat4 inv = glm::inverse(req.model->bind_pose[i]);
            const float* p = glm::value_ptr(inv);
            ibm.insert(ibm.end(), p, p + 16);
        }
        inv_bind_accessor = add_float_array(glb, ibm, 0, bone_count, "MAT4");
    }
    std::vector<SampledClip> sampled = sample_clips(req, bone_count, local_bind);
    std::vector<std::vector<int>> anim_accessors(sampled.size());
    for (size_t ai = 0; ai < sampled.size(); ++ai) {
        const auto& sc = sampled[ai];
        std::vector<double> minv = sc.times.empty() ? std::vector<double>{0.0} : std::vector<double>{sc.times.front()};
        std::vector<double> maxv = sc.times.empty() ? std::vector<double>{0.0} : std::vector<double>{sc.times.back()};
        int time_acc = add_float_array(glb, sc.times, 0, sc.times.size(), "SCALAR", minv, maxv);
        anim_accessors[ai].push_back(time_acc);
        for (int b = 0; b < bone_count; ++b) {
            std::vector<float> qv;
            qv.reserve(sc.times.size() * 4u);
            for (glm::quat q : sc.rotations[b]) {
                q = safe_quat(q);
                qv.push_back(q.x);
                qv.push_back(q.y);
                qv.push_back(q.z);
                qv.push_back(q.w);
            }
            anim_accessors[ai].push_back(add_float_array(glb, qv, 0, sc.times.size(), "VEC4"));
        }
    }
    if (prims.empty()) {
        error = "Model has no exportable primitives";
        return false;
    }
    std::string json = build_glb_json(req, glb, prims, textures, mat_texture, bone_count,
                                      local_bind, inv_bind_accessor, sampled, anim_accessors);
    if (!write_glb_file(out, json, glb.bin)) {
        error = "Could not write GLB";
        return false;
    }
    return true;
}
