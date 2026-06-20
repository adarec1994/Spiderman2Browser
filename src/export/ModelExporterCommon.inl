struct TextureRef {
    int image_index = -1;
    int texture_index = -1;
    std::string uri;
    std::string absolute_path;
};

struct BufferView {
    size_t offset = 0;
    size_t length = 0;
    int target = 0;
};

struct Accessor {
    int view = -1;
    int component_type = 5126;
    size_t count = 0;
    std::string type;
    std::vector<double> minv;
    std::vector<double> maxv;
};

struct PrimitiveDef {
    int pos = -1;
    int uv = -1;
    int joints = -1;
    int weights = -1;
    int indices = -1;
    int material = -1;
};

struct SampledClip {
    std::string name;
    std::vector<float> times;
    std::vector<std::vector<glm::quat>> rotations;
};

struct GlbData {
    std::vector<uint8_t> bin;
    std::vector<BufferView> views;
    std::vector<Accessor> accessors;

    void align4() {
        while (bin.size() & 3u) bin.push_back(0);
    }

    int add_raw(const void* ptr, size_t bytes, int target, int component_type,
                size_t count, const std::string& type,
                std::vector<double> minv = {}, std::vector<double> maxv = {}) {
        align4();
        BufferView view;
        view.offset = bin.size();
        view.length = bytes;
        view.target = target;
        const uint8_t* b = static_cast<const uint8_t*>(ptr);
        bin.insert(bin.end(), b, b + bytes);
        views.push_back(view);
        Accessor acc;
        acc.view = (int)views.size() - 1;
        acc.component_type = component_type;
        acc.count = count;
        acc.type = type;
        acc.minv = std::move(minv);
        acc.maxv = std::move(maxv);
        accessors.push_back(std::move(acc));
        return (int)accessors.size() - 1;
    }
};

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static std::string json_escape(const std::string& s) {
    std::ostringstream o;
    for (char ch : s) {
        unsigned char c = (unsigned char)ch;
        if (ch == '\\') o << "\\\\";
        else if (ch == '"') o << "\\\"";
        else if (ch == '\n') o << "\\n";
        else if (ch == '\r') o << "\\r";
        else if (ch == '\t') o << "\\t";
        else if (c < 32) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
        else o << ch;
    }
    return o.str();
}

static std::string clean_name(std::string s) {
    if (s.empty()) s = "asset";
    for (char& c : s) {
        unsigned char uc = (unsigned char)c;
        if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.') c = '_';
    }
    while (!s.empty() && s.front() == '.') s.erase(s.begin());
    if (s.empty()) s = "asset";
    return s;
}

static glm::quat safe_quat(glm::quat q) {
    float m2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    if (!std::isfinite(m2) || m2 <= 1e-12f) return glm::quat(1, 0, 0, 0);
    return q * (1.0f / std::sqrt(m2));
}

static std::string strip_instance_suffix(std::string stem) {
    if (stem.size() > 9) {
        std::string tail = stem.substr(stem.size() - 9);
        if (tail[0] == '_' && std::all_of(tail.begin() + 1, tail.end(),
                                          [](unsigned char c){ return std::isdigit(c); }))
            return stem.substr(0, stem.size() - 9);
    }
    return stem;
}

static bool write_binary(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return (bool)f;
}

static uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static void color565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (uint8_t)((((c >> 11) & 31) * 255 + 15) / 31);
    g = (uint8_t)((((c >> 5) & 63) * 255 + 31) / 63);
    b = (uint8_t)(((c & 31) * 255 + 15) / 31);
}

static void put_px(std::vector<uint8_t>& out, int w, int h, int x, int y, const uint8_t* rgba) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    size_t off = ((size_t)y * (size_t)w + (size_t)x) * 4u;
    out[off + 0] = rgba[0];
    out[off + 1] = rgba[1];
    out[off + 2] = rgba[2];
    out[off + 3] = rgba[3];
}

static bool decode_dds(const std::vector<uint8_t>& file, int& width, int& height, std::vector<uint8_t>& rgba) {
    if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0) return false;
    const uint8_t* hdr = file.data() + 4;
    height = (int)rd32(hdr + 8);
    width = (int)rd32(hdr + 12);
    uint32_t four = rd32(hdr + 80);
    if (width <= 0 || height <= 0) return false;
    char cc[5] = {};
    std::memcpy(cc, &four, 4);
    bool dxt1 = std::memcmp(cc, "DXT1", 4) == 0;
    bool dxt3 = std::memcmp(cc, "DXT3", 4) == 0;
    bool dxt5 = std::memcmp(cc, "DXT5", 4) == 0;
    if (!dxt1 && !dxt3 && !dxt5) return false;
    rgba.assign((size_t)width * (size_t)height * 4u, 0);
    size_t pos = 128;
    int bw = (width + 3) / 4;
    int bh = (height + 3) / 4;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            uint8_t alpha[16];
            std::fill(std::begin(alpha), std::end(alpha), 255);
            if (dxt3) {
                if (pos + 8 > file.size()) return false;
                uint64_t ab = 0;
                std::memcpy(&ab, file.data() + pos, 8);
                pos += 8;
                for (int i = 0; i < 16; ++i)
                    alpha[i] = (uint8_t)((((ab >> (i * 4)) & 15) * 17) & 255);
            } else if (dxt5) {
                if (pos + 8 > file.size()) return false;
                uint8_t a0 = file[pos + 0];
                uint8_t a1 = file[pos + 1];
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i)
                    bits |= (uint64_t)file[pos + 2 + i] << (8 * i);
                pos += 8;
                uint8_t table[8] = {};
                table[0] = a0;
                table[1] = a1;
                if (a0 > a1) {
                    table[2] = (uint8_t)((6*a0 + 1*a1 + 3) / 7);
                    table[3] = (uint8_t)((5*a0 + 2*a1 + 3) / 7);
                    table[4] = (uint8_t)((4*a0 + 3*a1 + 3) / 7);
                    table[5] = (uint8_t)((3*a0 + 4*a1 + 3) / 7);
                    table[6] = (uint8_t)((2*a0 + 5*a1 + 3) / 7);
                    table[7] = (uint8_t)((1*a0 + 6*a1 + 3) / 7);
                } else {
                    table[2] = (uint8_t)((4*a0 + 1*a1 + 2) / 5);
                    table[3] = (uint8_t)((3*a0 + 2*a1 + 2) / 5);
                    table[4] = (uint8_t)((2*a0 + 3*a1 + 2) / 5);
                    table[5] = (uint8_t)((1*a0 + 4*a1 + 2) / 5);
                    table[6] = 0;
                    table[7] = 255;
                }
                for (int i = 0; i < 16; ++i)
                    alpha[i] = table[(bits >> (3 * i)) & 7u];
            }
            if (pos + 8 > file.size()) return false;
            uint16_t c0 = rd16(file.data() + pos);
            uint16_t c1 = rd16(file.data() + pos + 2);
            uint32_t bits = rd32(file.data() + pos + 4);
            pos += 8;
            uint8_t cols[4][4] = {};
            color565(c0, cols[0][0], cols[0][1], cols[0][2]);
            color565(c1, cols[1][0], cols[1][1], cols[1][2]);
            cols[0][3] = cols[1][3] = 255;
            if (c0 > c1 || !dxt1) {
                for (int k = 0; k < 3; ++k) {
                    cols[2][k] = (uint8_t)((2 * cols[0][k] + cols[1][k] + 1) / 3);
                    cols[3][k] = (uint8_t)((cols[0][k] + 2 * cols[1][k] + 1) / 3);
                }
                cols[2][3] = cols[3][3] = 255;
            } else {
                for (int k = 0; k < 3; ++k)
                    cols[2][k] = (uint8_t)((cols[0][k] + cols[1][k]) / 2);
                cols[2][3] = 255;
                cols[3][3] = 0;
            }
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    int i = py * 4 + px;
                    uint8_t idx = (uint8_t)((bits >> (2 * i)) & 3u);
                    uint8_t p[4] = { cols[idx][0], cols[idx][1], cols[idx][2], (uint8_t)(cols[idx][3] * alpha[i] / 255) };
                    put_px(rgba, width, height, bx * 4 + px, by * 4 + py, p);
                }
            }
        }
    }
    return true;
}

static bool write_png_rgba(const fs::path& path, int w, int h, const std::vector<uint8_t>& rgba) {
    return stbi_write_png(path.string().c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

static bool export_texture_file(const std::string& source, const fs::path& out_dir,
                                const std::string& base, TextureRef& ref) {
    std::vector<uint8_t> data = vfs::read_file(source);
    if (data.empty()) return false;
    std::string ext = lower_copy(fs::path(source).extension().string());
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) return false;
    std::string name = clean_name(base.empty() ? fs::path(source).stem().string() : base);
    fs::path dst;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
        dst = out_dir / (name + ext);
        if (!write_binary(dst, data)) return false;
    } else {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        bool ok = false;
        if (ext == ".dds")
            ok = decode_dds(data, w, h, rgba);
        if (!ok) {
            int ch = 0;
            unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &ch, 4);
            if (pixels) {
                rgba.assign(pixels, pixels + (size_t)w * (size_t)h * 4u);
                stbi_image_free(pixels);
                ok = true;
            }
        }
        if (!ok) {
            dst = out_dir / (name + ext);
            if (!write_binary(dst, data)) return false;
        } else {
            for (size_t i = 3; i < rgba.size(); i += 4)
                rgba[i] = 255;
            dst = out_dir / (name + ".png");
            if (!write_png_rgba(dst, w, h, rgba)) return false;
        }
    }
    ref.absolute_path = fs::absolute(dst).string();
    ref.uri = out_dir.filename().generic_string() + "/" + dst.filename().generic_string();
    return true;
}

static std::vector<glm::mat4> local_bind_matrices(const ExportRequest& req, int bone_count) {
    std::vector<glm::mat4> local(bone_count, glm::mat4(1.0f));
    for (int i = 0; i < bone_count; ++i) {
        int parent = req.skeleton ? req.skeleton->bones[i].parent : -1;
        if (parent >= 0 && parent < (int)req.model->bind_pose.size())
            local[i] = glm::inverse(req.model->bind_pose[parent]) * req.model->bind_pose[i];
        else if (i < (int)req.model->bind_pose.size())
            local[i] = req.model->bind_pose[i];
    }
    return local;
}

static float minion_weight(int i) {
    switch (i) {
        case 10: case 22:
            return 0.80f;
        case 11: case 12: case 13: case 14:
        case 15: case 16: case 17: case 18:
        case 23: case 24: case 25: case 26:
        case 27: case 28: case 29: case 30:
            return 0.45f;
        case 32: case 38:
            return 0.70f;
        case 33: case 34: case 35: case 36:
        case 39: case 40: case 41: case 42:
            return 0.62f;
        default:
            return 1.0f;
    }
}

static glm::quat root_ref_for_clips(std::vector<AnimClip>* clips) {
    if (!clips || clips->empty()) return glm::quat(1, 0, 0, 0);
    int idx = -1;
    for (int i = 0; i < (int)clips->size(); ++i) {
        std::string name = lower_copy((*clips)[i].name);
        if (name.find("idl") != std::string::npos) {
            idx = i;
            break;
        }
    }
    if (idx < 0) idx = 0;
    const AnimClip& clip = (*clips)[idx];
    if (!clip.loaded) return glm::quat(1, 0, 0, 0);
    auto poses = clip.sample_pose(0.0f);
    if (poses.empty()) return glm::quat(1, 0, 0, 0);
    return safe_quat(poses[0].q);
}

static std::vector<SampledClip> sample_clips(const ExportRequest& req, int bone_count,
                                             const std::vector<glm::mat4>& local_bind) {
    std::vector<SampledClip> out;
    if (!req.animations || !req.skeleton || bone_count <= 0) return out;
    glm::quat root_ref = root_ref_for_clips(req.animations);
    for (const AnimClip& clip : *req.animations) {
        if (!clip.loaded || clip.frame_count <= 0) continue;
        float dur = clip.duration > 0.0f ? clip.duration
                  : (clip.fps > 0.0f && clip.frame_count > 1
                     ? (float)(clip.frame_count - 1) / clip.fps : 0.0f);
        bool looping = clip.looping && dur > 0.0f && clip.frame_count > 1;
        int samples = looping ? clip.frame_count + 1 : std::max(clip.frame_count, 1);
        if (dur <= 0.0f) samples = 1;
        SampledClip sampled;
        sampled.name = clip.name;
        sampled.times.resize(samples);
        sampled.rotations.assign(bone_count, std::vector<glm::quat>(samples, glm::quat(1, 0, 0, 0)));
        for (int s = 0; s < samples; ++s) {
            float t = 0.0f;
            if (samples > 1 && dur > 0.0f)
                t = looping ? dur * (float)s / (float)clip.frame_count
                            : dur * (float)s / (float)(samples - 1);
            sampled.times[s] = t;
            auto poses = clip.sample_pose(t);
            std::vector<BonePose> by_skel(bone_count);
            std::vector<bool> has_pose(bone_count, false);
            for (int ai = 0; ai < (int)poses.size(); ++ai) {
                int si = ai < (int)clip.bone_indices.size() ? clip.bone_indices[ai] : ai;
                if (si >= 0 && si < bone_count) {
                    by_skel[si] = poses[ai];
                    has_pose[si] = true;
                }
            }
            for (int i = 0; i < bone_count; ++i) {
                int parent = req.skeleton->bones[i].parent;
                glm::quat bind_q = safe_quat(glm::quat_cast(glm::mat3(local_bind[i])));
                glm::quat q = bind_q;
                if (has_pose[i]) {
                    float qmag = by_skel[i].q.x*by_skel[i].q.x + by_skel[i].q.y*by_skel[i].q.y +
                                 by_skel[i].q.z*by_skel[i].q.z + by_skel[i].q.w*by_skel[i].q.w;
                    if (std::isfinite(qmag) && qmag >= 1e-8f) {
                        glm::quat anim_q = safe_quat(by_skel[i].q);
                        q = parent < 0 ? safe_quat((bind_q * glm::inverse(root_ref)) * anim_q) : anim_q;
                    }
                }
                if (req.minion_lizard) {
                    float w = minion_weight(i);
                    if (w < 1.0f) {
                        if (glm::dot(bind_q, q) < 0.0f) q = -q;
                        q = safe_quat(glm::slerp(bind_q, q, w));
                    }
                }
                sampled.rotations[i][s] = safe_quat(q);
            }
        }
        out.push_back(std::move(sampled));
    }
    return out;
}

static int add_float_array(GlbData& glb, const std::vector<float>& values, int target,
                           size_t count, const std::string& type,
                           std::vector<double> minv = {}, std::vector<double> maxv = {}) {
    return glb.add_raw(values.data(), values.size() * sizeof(float), target, 5126,
                       count, type, std::move(minv), std::move(maxv));
}

static int add_u32_array(GlbData& glb, const std::vector<uint32_t>& values, int target,
                         size_t count, const std::string& type) {
    return glb.add_raw(values.data(), values.size() * sizeof(uint32_t), target, 5125, count, type);
}

static int add_u16_array(GlbData& glb, const std::vector<uint16_t>& values, int target,
                         size_t count, const std::string& type) {
    return glb.add_raw(values.data(), values.size() * sizeof(uint16_t), target, 5123, count, type);
}

static std::vector<double> min3(const std::vector<glm::vec3>& v) {
    glm::vec3 m(1e30f);
    for (auto& p : v) m = glm::min(m, p);
    return {m.x, m.y, m.z};
}

static std::vector<double> max3(const std::vector<glm::vec3>& v) {
    glm::vec3 m(-1e30f);
    for (auto& p : v) m = glm::max(m, p);
    return {m.x, m.y, m.z};
}

static std::string number(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(9) << v;
    return o.str();
}

static void write_num_array(std::ostringstream& o, const std::vector<double>& vals) {
    o << "[";
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i) o << ",";
        o << number(vals[i]);
    }
    o << "]";
}

static std::vector<TextureRef> export_material_textures(const ExportRequest& req,
                                                        std::vector<int>& mat_texture,
                                                        std::string& error) {
    std::vector<TextureRef> refs;
    mat_texture.assign(req.model->submeshes.size(), -1);
    fs::path out = req.output_path;
    fs::path tex_dir = out.parent_path() / (out.stem().string() + "_textures");
    std::string model_dir = fs::path(req.model->filepath).parent_path().string();
    std::unordered_map<std::string, int> by_src;
    for (int i = 0; i < (int)req.model->submeshes.size(); ++i) {
        const auto& sm = req.model->submeshes[i];
        std::string src = resolve_texture_path(sm.tex_candidates, model_dir);
        if (src.empty()) continue;
        auto found = by_src.find(src);
        if (found != by_src.end()) {
            mat_texture[i] = found->second;
            continue;
        }
        TextureRef ref;
        std::string tex_stem = fs::path(src).stem().string();
        std::string mat_stem = sm.mat_name.empty() ? ("submesh_" + std::to_string(i)) : sm.mat_name;
        std::string base = "sm" + std::to_string(i) + "_" + mat_stem + "_" + tex_stem;
        if (!export_texture_file(src, tex_dir, base, ref)) {
            error = "Could not export texture " + src;
            continue;
        }
        ref.image_index = (int)refs.size();
        ref.texture_index = (int)refs.size();
        by_src[src] = (int)refs.size();
        mat_texture[i] = (int)refs.size();
        refs.push_back(std::move(ref));
    }
    return refs;
}
