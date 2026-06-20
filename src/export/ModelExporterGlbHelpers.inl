static std::string build_glb_json(const ExportRequest& req, const GlbData& glb,
                                  const std::vector<PrimitiveDef>& prims,
                                  const std::vector<TextureRef>& tex_refs,
                                  const std::vector<int>& mat_texture,
                                  int bone_count,
                                  const std::vector<glm::mat4>& local_bind,
                                  int inv_bind_accessor,
                                  const std::vector<SampledClip>& sampled,
                                  const std::vector<std::vector<int>>& anim_accessors) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Spiderman 2 Asset Browser\"},";
    o << "\"buffers\":[{\"byteLength\":" << glb.bin.size() << "}],";
    o << "\"bufferViews\":[";
    for (size_t i = 0; i < glb.views.size(); ++i) {
        if (i) o << ",";
        const auto& v = glb.views[i];
        o << "{\"buffer\":0,\"byteOffset\":" << v.offset << ",\"byteLength\":" << v.length;
        if (v.target) o << ",\"target\":" << v.target;
        o << "}";
    }
    o << "],\"accessors\":[";
    for (size_t i = 0; i < glb.accessors.size(); ++i) {
        if (i) o << ",";
        const auto& a = glb.accessors[i];
        o << "{\"bufferView\":" << a.view << ",\"componentType\":" << a.component_type
          << ",\"count\":" << a.count << ",\"type\":\"" << a.type << "\"";
        if (!a.minv.empty()) {
            o << ",\"min\":";
            write_num_array(o, a.minv);
        }
        if (!a.maxv.empty()) {
            o << ",\"max\":";
            write_num_array(o, a.maxv);
        }
        o << "}";
    }
    o << "],";
    if (!tex_refs.empty()) {
        o << "\"images\":[";
        for (size_t i = 0; i < tex_refs.size(); ++i) {
            if (i) o << ",";
            o << "{\"uri\":\"" << json_escape(tex_refs[i].uri) << "\"}";
        }
        o << "],\"textures\":[";
        for (size_t i = 0; i < tex_refs.size(); ++i) {
            if (i) o << ",";
            o << "{\"source\":" << i << "}";
        }
        o << "],";
    }
    o << "\"materials\":[";
    for (size_t i = 0; i < req.model->submeshes.size(); ++i) {
        if (i) o << ",";
        const auto& sm = req.model->submeshes[i];
        o << "{\"name\":\"" << json_escape(sm.mat_name.empty() ? ("material_" + std::to_string(i)) : sm.mat_name) << "\"";
        int ti = i < mat_texture.size() ? mat_texture[i] : -1;
        o << ",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0,\"roughnessFactor\":1";
        if (ti >= 0) o << ",\"baseColorTexture\":{\"index\":" << tex_refs[ti].texture_index << "}";
        o << "}";
        std::string sh = lower_copy(sm.shader_type + " " + sm.mat_name);
        if (sh.find("translucent") != std::string::npos || sh.find("glass") != std::string::npos ||
            sh.find("fxenv") != std::string::npos || sh.ends_with("_a"))
            o << ",\"alphaMode\":\"BLEND\",\"doubleSided\":true";
        else
            o << ",\"doubleSided\":true";
        o << "}";
    }
    o << "],\"meshes\":[{\"name\":\"" << json_escape(fs::path(req.model->filepath).stem().string()) << "\",\"primitives\":[";
    for (size_t i = 0; i < prims.size(); ++i) {
        if (i) o << ",";
        const auto& p = prims[i];
        o << "{\"attributes\":{\"POSITION\":" << p.pos;
        if (p.uv >= 0) o << ",\"TEXCOORD_0\":" << p.uv;
        if (p.joints >= 0) o << ",\"JOINTS_0\":" << p.joints;
        if (p.weights >= 0) o << ",\"WEIGHTS_0\":" << p.weights;
        o << "},\"indices\":" << p.indices << ",\"material\":" << p.material << ",\"mode\":4}";
    }
    o << "]}],\"nodes\":[";
    std::vector<std::vector<int>> children(bone_count);
    for (int i = 0; i < bone_count; ++i) {
        int p = req.skeleton ? req.skeleton->bones[i].parent : -1;
        if (p >= 0 && p < bone_count) children[p].push_back(i);
    }
    for (int i = 0; i < bone_count; ++i) {
        if (i) o << ",";
        std::string name = req.skeleton ? req.skeleton->bones[i].name : ("bone_" + std::to_string(i));
        glm::vec3 t(local_bind[i][3]);
        glm::quat q = safe_quat(glm::quat_cast(glm::mat3(local_bind[i])));
        o << "{\"name\":\"" << json_escape(name) << "\",\"translation\":["
          << number(t.x) << "," << number(t.y) << "," << number(t.z) << "],\"rotation\":["
          << number(q.x) << "," << number(q.y) << "," << number(q.z) << "," << number(q.w) << "]";
        if (!children[i].empty()) {
            o << ",\"children\":[";
            for (size_t c = 0; c < children[i].size(); ++c) {
                if (c) o << ",";
                o << children[i][c];
            }
            o << "]";
        }
        o << "}";
    }
    if (bone_count) o << ",";
    o << "{\"name\":\"" << json_escape(fs::path(req.model->filepath).stem().string()) << "\",\"mesh\":0";
    if (bone_count) o << ",\"skin\":0";
    o << "}";
    o << "],";
    if (bone_count) {
        o << "\"skins\":[{\"inverseBindMatrices\":" << inv_bind_accessor << ",\"joints\":[";
        for (int i = 0; i < bone_count; ++i) {
            if (i) o << ",";
            o << i;
        }
        o << "],\"skeleton\":0}],";
    }
    if (!sampled.empty()) {
        o << "\"animations\":[";
        for (size_t ai = 0; ai < sampled.size(); ++ai) {
            if (ai) o << ",";
            o << "{\"name\":\"" << json_escape(sampled[ai].name) << "\",\"samplers\":[";
            int sampler_count = bone_count;
            for (int b = 0; b < sampler_count; ++b) {
                if (b) o << ",";
                o << "{\"input\":" << anim_accessors[ai][0] << ",\"output\":" << anim_accessors[ai][b + 1]
                  << ",\"interpolation\":\"LINEAR\"}";
            }
            o << "],\"channels\":[";
            for (int b = 0; b < sampler_count; ++b) {
                if (b) o << ",";
                o << "{\"sampler\":" << b << ",\"target\":{\"node\":" << b << ",\"path\":\"rotation\"}}";
            }
            o << "]}";
        }
        o << "],";
    }
    o << "\"scenes\":[{\"nodes\":[";
    bool first = true;
    for (int i = 0; i < bone_count; ++i) {
        int p = req.skeleton ? req.skeleton->bones[i].parent : -1;
        if (p < 0 || p >= bone_count) {
            if (!first) o << ",";
            first = false;
            o << i;
        }
    }
    if (!first) o << ",";
    o << bone_count << "]}],\"scene\":0}";
    return o.str();
}

static bool write_glb_file(const fs::path& path, const std::string& json, std::vector<uint8_t> bin) {
    std::string j = json;
    while (j.size() & 3u) j.push_back(' ');
    while (bin.size() & 3u) bin.push_back(0);
    uint32_t total = 12u + 8u + (uint32_t)j.size() + 8u + (uint32_t)bin.size();
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto wr32 = [&](uint32_t v){ f.write(reinterpret_cast<const char*>(&v), 4); };
    wr32(0x46546C67u);
    wr32(2u);
    wr32(total);
    wr32((uint32_t)j.size());
    wr32(0x4E4F534Au);
    f.write(j.data(), (std::streamsize)j.size());
    wr32((uint32_t)bin.size());
    wr32(0x004E4942u);
    if (!bin.empty()) f.write(reinterpret_cast<const char*>(bin.data()), (std::streamsize)bin.size());
    return (bool)f;
}
