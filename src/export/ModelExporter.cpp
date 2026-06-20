#include "ModelExporter.h"
#include "Texture.h"
#include "Vfs.h"

#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace model_export {
namespace {

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

static void fbx_array(std::ostringstream& o, const std::string& name, const std::vector<double>& vals) {
    o << "\t\t" << name << ": *" << vals.size() << " {\n\t\t\ta: ";
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i) o << ",";
        o << number(vals[i]);
    }
    o << "\n\t\t}\n";
}

static void fbx_array_i(std::ostringstream& o, const std::string& name, const std::vector<int64_t>& vals) {
    o << "\t\t" << name << ": *" << vals.size() << " {\n\t\t\ta: ";
    for (size_t i = 0; i < vals.size(); ++i) {
        if (i) o << ",";
        o << vals[i];
    }
    o << "\n\t\t}\n";
}

struct FbxTRS {
    glm::vec3 t{0.0f};
    glm::vec3 r_deg{0.0f};
    glm::vec3 s{1.0f};
    glm::mat4 matrix{1.0f};
};

static glm::vec3 extract_xyz_degrees(const glm::mat4& m) {
    float t1 = std::atan2(m[2][1], m[2][2]);
    float c2 = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
    float t2 = std::atan2(-m[2][0], c2);
    float s1 = std::sin(t1);
    float c1 = std::cos(t1);
    float t3 = std::atan2(s1 * m[0][2] - c1 * m[0][1],
                          c1 * m[1][1] - s1 * m[1][2]);
    return glm::degrees(glm::vec3(-t1, -t2, -t3));
}

static glm::mat3 orthonormal_basis(glm::vec3 x, glm::vec3 y, glm::vec3 z) {
    if (glm::dot(x, x) < 1e-12f) x = glm::vec3(1, 0, 0); else x = glm::normalize(x);
    y = y - x * glm::dot(x, y);
    if (glm::dot(y, y) < 1e-12f) {
        y = glm::abs(x.y) < 0.9f ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), x))
                                 : glm::normalize(glm::cross(glm::vec3(1, 0, 0), x));
    } else {
        y = glm::normalize(y);
    }
    z = glm::cross(x, y);
    if (glm::dot(z, z) < 1e-12f) z = glm::vec3(0, 0, 1); else z = glm::normalize(z);
    y = glm::normalize(glm::cross(z, x));
    return glm::mat3(x, y, z);
}

static FbxTRS decompose_fbx_trs(const glm::mat4& m) {
    FbxTRS out;
    out.t = glm::vec3(m[3]);
    glm::vec3 x(m[0]);
    glm::vec3 y(m[1]);
    glm::vec3 z(m[2]);
    out.s = glm::vec3(std::sqrt(std::max(0.0f, glm::dot(x, x))),
                      std::sqrt(std::max(0.0f, glm::dot(y, y))),
                      std::sqrt(std::max(0.0f, glm::dot(z, z))));
    if (out.s.x < 1e-8f) out.s.x = 1.0f;
    if (out.s.y < 1e-8f) out.s.y = 1.0f;
    if (out.s.z < 1e-8f) out.s.z = 1.0f;
    glm::mat3 basis(x / out.s.x, y / out.s.y, z / out.s.z);
    if (glm::determinant(basis) < 0.0f) {
        out.s.x = -out.s.x;
        basis[0] = -basis[0];
    }
    basis = orthonormal_basis(basis[0], basis[1], basis[2]);
    glm::mat4 r(1.0f);
    r[0] = glm::vec4(basis[0], 0.0f);
    r[1] = glm::vec4(basis[1], 0.0f);
    r[2] = glm::vec4(basis[2], 0.0f);
    out.r_deg = extract_xyz_degrees(r);
    out.matrix = glm::mat4(1.0f);
    out.matrix[0] = glm::vec4(basis[0] * out.s.x, 0.0f);
    out.matrix[1] = glm::vec4(basis[1] * out.s.y, 0.0f);
    out.matrix[2] = glm::vec4(basis[2] * out.s.z, 0.0f);
    out.matrix[3] = glm::vec4(out.t, 1.0f);
    return out;
}

static FbxTRS fbx_identity_trs() {
    return decompose_fbx_trs(glm::mat4(1.0f));
}

static glm::mat4 fbx_export_root_matrix() {
    glm::mat4 m(1.0f);
    m[1][1] = -1.0f;
    m[2][2] = -1.0f;
    return m;
}

static FbxTRS fbx_export_root_trs() {
    return decompose_fbx_trs(fbx_export_root_matrix());
}

static FbxTRS fbx_quat_trs(const glm::quat& q) {
    return decompose_fbx_trs(glm::mat4_cast(safe_quat(q)));
}

static double unwrap_degrees(double value, double reference) {
    while (value - reference > 180.0) value -= 360.0;
    while (value - reference < -180.0) value += 360.0;
    return value;
}

static std::vector<glm::vec3> fbx_euler_curve(const std::vector<glm::quat>& rotations) {
    std::vector<glm::vec3> out;
    out.reserve(rotations.size());
    glm::quat prev_q(1, 0, 0, 0);
    glm::vec3 prev_e(0.0f);
    bool first = true;
    for (glm::quat q : rotations) {
        q = safe_quat(q);
        if (!first && glm::dot(prev_q, q) < 0.0f)
            q = -q;
        glm::vec3 base = fbx_quat_trs(q).r_deg;
        glm::vec3 e = base;
        if (!first) {
            std::array<glm::vec3, 2> branches = {
                base,
                glm::vec3(base.x + 180.0f, 180.0f - base.y, base.z + 180.0f)
            };
            double best = std::numeric_limits<double>::max();
            for (glm::vec3 cand : branches) {
                cand.x = (float)unwrap_degrees(cand.x, prev_e.x);
                cand.y = (float)unwrap_degrees(cand.y, prev_e.y);
                cand.z = (float)unwrap_degrees(cand.z, prev_e.z);
                glm::vec3 d = cand - prev_e;
                double score = (double)d.x * d.x + (double)d.y * d.y + (double)d.z * d.z;
                if (score < best) {
                    best = score;
                    e = cand;
                }
            }
        }
        out.push_back(e);
        prev_q = q;
        prev_e = e;
        first = false;
    }
    return out;
}

static void fbx_props_transform(std::ostringstream& o, const FbxTRS& trs) {
    o << "\t\tProperties70:  {\n";
    o << "\t\t\tP: \"RotationActive\", \"bool\", \"\", \"\",1\n";
    o << "\t\t\tP: \"RotationOrder\", \"enum\", \"\", \"\",0\n";
    o << "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n";
    o << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\","
      << number(trs.t.x) << "," << number(trs.t.y) << "," << number(trs.t.z) << "\n";
    o << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\","
      << number(trs.r_deg.x) << "," << number(trs.r_deg.y) << "," << number(trs.r_deg.z) << "\n";
    o << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\","
      << number(trs.s.x) << "," << number(trs.s.y) << "," << number(trs.s.z) << "\n";
    o << "\t\t}\n";
}

static std::vector<glm::mat4> exported_world_bind(const ExportRequest& req,
                                                  const std::vector<glm::mat4>& local_bind,
                                                  int bone_count) {
    std::vector<glm::mat4> local_fbx(bone_count, glm::mat4(1.0f));
    std::vector<glm::mat4> world(bone_count, glm::mat4(1.0f));
    for (int i = 0; i < bone_count; ++i)
        local_fbx[i] = decompose_fbx_trs(local_bind[i]).matrix;
    for (int i = 0; i < bone_count; ++i) {
        int parent = req.skeleton ? req.skeleton->bones[i].parent : -1;
        if (parent >= 0 && parent < i)
            world[i] = world[parent] * local_fbx[i];
        else
            world[i] = local_fbx[i];
    }
    return world;
}

static std::vector<double> fbx_matrix(const glm::mat4& m) {
    std::vector<double> v;
    v.reserve(16);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            v.push_back(m[c][r]);
    return v;
}

}

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

bool export_fbx(const ExportRequest& req, std::string& error) {
    error.clear();
    if (!req.model || req.output_path.empty()) {
        error = "No model or output path";
        return false;
    }
    fs::path out = req.output_path;
    std::error_code ec;
    if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
    std::vector<int> mat_texture;
    std::vector<TextureRef> textures = export_material_textures(req, mat_texture, error);
    if (!error.empty()) return false;
    int bone_count = 0;
    if (req.skeleton && req.model->bind_pose.size() > 0)
        bone_count = std::min({(int)req.skeleton->bones.size(), (int)req.model->bind_pose.size(), 60});
    std::vector<glm::mat4> local_bind = local_bind_matrices(req, bone_count);
    glm::mat4 fbx_root_matrix = fbx_export_root_matrix();
    std::vector<glm::mat4> fbx_bind_world(bone_count, glm::mat4(1.0f));
    for (int i = 0; i < bone_count; ++i)
        fbx_bind_world[i] = fbx_root_matrix * req.model->bind_pose[i];
    std::vector<SampledClip> sampled = sample_clips(req, bone_count, local_bind);

    struct FbxFace {
        uint32_t v[3] = {};
        glm::vec2 uv[3] = {};
    };
    struct FbxSubmesh {
        int source_index = -1;
        std::string name;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normal_accum;
        std::vector<FbxFace> faces;
        std::vector<double> verts;
        std::vector<int64_t> poly;
        std::vector<double> normals;
        std::vector<double> uvs;
        std::vector<int64_t> uv_idx;
        std::vector<glm::ivec4> vertex_joints;
        std::vector<glm::vec4> vertex_weights;
        std::vector<std::vector<int64_t>> cluster_indexes;
        std::vector<std::vector<double>> cluster_weights;
        std::vector<int64_t> cluster_ids;
        int64_t geom_id = 0;
        int64_t mesh_id = 0;
        int64_t skin_id = 0;
        int64_t material_id = 0;
    };
    std::vector<FbxSubmesh> fbx_submeshes;

    for (int si = 0; si < (int)req.model->submeshes.size(); ++si) {
        const auto& sm = req.model->submeshes[si];
        if (sm.positions.empty() || sm.indices.empty()) continue;
        FbxSubmesh out_sm;
        out_sm.source_index = si;
        out_sm.name = clean_name(fs::path(req.model->filepath).stem().string() + "_SM" + std::to_string(si) + "_" +
                                 (sm.mat_name.empty() ? std::string("material") : sm.mat_name));
        out_sm.positions.reserve(sm.positions.size());
        out_sm.normal_accum.assign(sm.positions.size(), glm::vec3(0.0f));
        out_sm.vertex_joints.reserve(sm.positions.size());
        out_sm.vertex_weights.reserve(sm.positions.size());
        for (size_t vi = 0; vi < sm.positions.size(); ++vi) {
            const glm::vec3& p = sm.positions[vi];
            out_sm.positions.push_back(p);
            out_sm.verts.push_back(p.x);
            out_sm.verts.push_back(p.y);
            out_sm.verts.push_back(p.z);
            if (bone_count > 0 && vi < sm.bone_indices.size() && vi < sm.bone_weights.size()) {
                glm::ivec4 ji = sm.bone_indices[vi];
                glm::vec4 jw = sm.bone_weights[vi];
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    ji[k] = std::clamp(ji[k], 0, bone_count - 1);
                    if (!std::isfinite(jw[k]) || jw[k] < 0.0f) jw[k] = 0.0f;
                    sum += jw[k];
                }
                if (sum > 0.0001f)
                    jw /= sum;
                else {
                    ji = glm::ivec4(0, 0, 0, 0);
                    jw = glm::vec4(1, 0, 0, 0);
                }
                out_sm.vertex_joints.push_back(ji);
                out_sm.vertex_weights.push_back(jw);
            } else {
                out_sm.vertex_joints.push_back(glm::ivec4(0, 0, 0, 0));
                out_sm.vertex_weights.push_back(glm::vec4(1, 0, 0, 0));
            }
        }
        for (size_t k = 0; k + 2 < sm.indices.size(); k += 3) {
            uint32_t ids[3] = {sm.indices[k], sm.indices[k + 1], sm.indices[k + 2]};
            FbxFace face;
            bool ok_face = true;
            for (int corner = 0; corner < 3; ++corner) {
                uint32_t idx = ids[corner];
                if (idx >= sm.positions.size()) {
                    ok_face = false;
                    break;
                }
                face.v[corner] = idx;
                glm::vec2 uv(0.0f);
                if (idx < sm.uvs.size()) uv = glm::vec2(sm.uvs[idx].x, 1.0f - sm.uvs[idx].y);
                face.uv[corner] = uv;
            }
            if (!ok_face) continue;
            glm::vec3 p0 = out_sm.positions[face.v[0]];
            glm::vec3 p1 = out_sm.positions[face.v[1]];
            glm::vec3 p2 = out_sm.positions[face.v[2]];
            glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
            if (std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z) &&
                glm::dot(n, n) > 1e-16f) {
                n = glm::normalize(n);
                out_sm.normal_accum[face.v[0]] += n;
                out_sm.normal_accum[face.v[1]] += n;
                out_sm.normal_accum[face.v[2]] += n;
            }
            out_sm.faces.push_back(face);
        }
        if (out_sm.faces.empty()) continue;
        for (glm::vec3& n : out_sm.normal_accum) {
            if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) ||
                glm::dot(n, n) <= 1e-16f)
                n = glm::vec3(0, 1, 0);
            else
                n = glm::normalize(n);
        }
        for (const FbxFace& face : out_sm.faces) {
            out_sm.poly.push_back((int64_t)face.v[0]);
            out_sm.poly.push_back((int64_t)face.v[1]);
            out_sm.poly.push_back(-(int64_t)face.v[2] - 1);
            for (int corner = 0; corner < 3; ++corner) {
                glm::vec3 n = out_sm.normal_accum[face.v[corner]];
                out_sm.normals.push_back(n.x);
                out_sm.normals.push_back(n.y);
                out_sm.normals.push_back(n.z);
                out_sm.uvs.push_back(face.uv[corner].x);
                out_sm.uvs.push_back(face.uv[corner].y);
                out_sm.uv_idx.push_back((int64_t)out_sm.uv_idx.size());
            }
        }
        out_sm.cluster_indexes.assign(bone_count, {});
        out_sm.cluster_weights.assign(bone_count, {});
        out_sm.cluster_ids.assign(bone_count, 0);
        if (bone_count > 0) {
            for (int vi = 0; vi < (int)out_sm.vertex_joints.size(); ++vi) {
                for (int bi = 0; bi < bone_count; ++bi) {
                    double w = 0.0;
                    for (int k = 0; k < 4; ++k)
                        if (out_sm.vertex_joints[vi][k] == bi)
                            w += out_sm.vertex_weights[vi][k];
                    if (w > 0.0001) {
                        out_sm.cluster_indexes[bi].push_back(vi);
                        out_sm.cluster_weights[bi].push_back(w);
                    }
                }
            }
        }
        fbx_submeshes.push_back(std::move(out_sm));
    }

    if (fbx_submeshes.empty()) {
        error = "Model has no exportable FBX geometry";
        return false;
    }

    std::ostringstream o;
    o.imbue(std::locale::classic());
    std::vector<std::string> conns;
    int64_t id = 100000;
    auto next_id = [&](){ return id++; };
    int64_t armature_id = next_id();
    int64_t armature_attr_id = next_id();
    int64_t bind_pose_id = bone_count > 0 ? next_id() : 0;
    std::vector<int64_t> bone_ids;
    std::vector<int64_t> bone_attr_ids;
    std::vector<int64_t> tex_ids(textures.size()), video_ids(textures.size());
    o << "; FBX 7.4.0 project file\n";
    o << "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n";
    o << "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n\t}\n}\n";
    o << "Objects:  {\n";

    o << "\tNodeAttribute: " << armature_attr_id << ", \"NodeAttribute::Armature\", \"Null\" {\n\t\tTypeFlags: \"Null\"\n\t}\n";
    o << "\tModel: " << armature_id << ", \"Model::Armature\", \"Null\" {\n\t\tVersion: 232\n";
    fbx_props_transform(o, fbx_export_root_trs());
    o << "\t}\n";
    conns.push_back("C: \"OO\"," + std::to_string(armature_attr_id) + "," + std::to_string(armature_id));
    conns.push_back("C: \"OO\"," + std::to_string(armature_id) + ",0");

    for (size_t i = 0; i < textures.size(); ++i) {
        video_ids[i] = next_id();
        tex_ids[i] = next_id();
        fs::path tp = textures[i].absolute_path;
        o << "\tVideo: " << video_ids[i] << ", \"Video::" << json_escape(tp.filename().string()) << "\", \"Clip\" {\n";
        o << "\t\tType: \"Clip\"\n\t\tFileName: \"" << json_escape(tp.string()) << "\"\n";
        o << "\t\tRelativeFilename: \"" << json_escape(textures[i].uri) << "\"\n\t\tUseMipMap: 0\n\t}\n";
        o << "\tTexture: " << tex_ids[i] << ", \"Texture::" << json_escape(tp.filename().string()) << "\", \"\" {\n";
        o << "\t\tType: \"TextureVideoClip\"\n\t\tVersion: 202\n\t\tTextureName: \"Texture::" << json_escape(tp.filename().string()) << "\"\n";
        o << "\t\tFileName: \"" << json_escape(tp.string()) << "\"\n\t\tRelativeFilename: \"" << json_escape(textures[i].uri) << "\"\n";
        o << "\t\tMedia: \"Video::" << json_escape(tp.filename().string()) << "\"\n";
        o << "\t\tTexture_Alpha_Source: \"None\"\n";
        o << "\t\tModelUVTranslation: 0,0\n\t\tModelUVScaling: 1,1\n\t\tCropping: 0,0,0,0\n";
        o << "\t\tProperties70:  {\n\t\t\tP: \"UVSet\", \"KString\", \"\", \"\", \"UVChannel_1\"\n\t\t\tP: \"UseMaterial\", \"bool\", \"\", \"\",1\n\t\t}\n\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(video_ids[i]) + "," + std::to_string(tex_ids[i]));
    }
    for (int i = 0; i < bone_count; ++i) {
        bone_ids.push_back(next_id());
        bone_attr_ids.push_back(next_id());
        std::string name = req.skeleton->bones[i].name.empty() ? ("bone_" + std::to_string(i)) : req.skeleton->bones[i].name;
        FbxTRS trs = decompose_fbx_trs(local_bind[i]);
        o << "\tNodeAttribute: " << bone_attr_ids.back() << ", \"NodeAttribute::" << json_escape(name) << "\", \"LimbNode\" {\n";
        o << "\t\tTypeFlags: \"Skeleton\"\n\t\tSkeletonType: \"LimbNode\"\n\t\tSize: 1\n\t}\n";
        o << "\tModel: " << bone_ids.back() << ", \"Model::" << json_escape(name) << "\", \"LimbNode\" {\n";
        o << "\t\tVersion: 232\n";
        fbx_props_transform(o, trs);
        o << "\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(bone_attr_ids[i]) + "," + std::to_string(bone_ids[i]));
        int p = req.skeleton->bones[i].parent;
        if (p >= 0 && p < i)
            conns.push_back("C: \"OO\"," + std::to_string(bone_ids[i]) + "," + std::to_string(bone_ids[p]));
        else
            conns.push_back("C: \"OO\"," + std::to_string(bone_ids[i]) + "," + std::to_string(armature_id));
    }

    for (FbxSubmesh& sm_out : fbx_submeshes) {
        sm_out.geom_id = next_id();
        sm_out.mesh_id = next_id();
        sm_out.material_id = next_id();
        if (bone_count > 0) {
            sm_out.skin_id = next_id();
            for (int bi = 0; bi < bone_count; ++bi)
                if (!sm_out.cluster_indexes[bi].empty())
                    sm_out.cluster_ids[bi] = next_id();
        }
    }

    for (FbxSubmesh& sm_out : fbx_submeshes) {
        const auto& src_sm = req.model->submeshes[sm_out.source_index];
        o << "\tGeometry: " << sm_out.geom_id << ", \"Geometry::" << sm_out.name << "\", \"Mesh\" {\n";
        fbx_array(o, "Vertices", sm_out.verts);
        fbx_array_i(o, "PolygonVertexIndex", sm_out.poly);
        o << "\t\tGeometryVersion: 124\n";
        o << "\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"Direct\"\n";
        fbx_array(o, "Normals", sm_out.normals);
        o << "\t\t}\n";
        o << "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVChannel_1\"\n\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"Direct\"\n";
        fbx_array(o, "UV", sm_out.uvs);
        o << "\t\t}\n";
        o << "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tMappingInformationType: \"AllSame\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n\t\t\tMaterials: *1 {\n\t\t\t\ta: 0\n\t\t\t}\n\t\t}\n";
        o << "\t\tLayer: 0 {\n\t\t\tVersion: 100\n\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n\t\t}\n";
        o << "\t}\n";
        o << "\tModel: " << sm_out.mesh_id << ", \"Model::" << sm_out.name << "\", \"Mesh\" {\n\t\tVersion: 232\n";
        fbx_props_transform(o, fbx_identity_trs());
        o << "\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n";
        std::string mat_name = clean_name(src_sm.mat_name.empty() ? ("material_" + std::to_string(sm_out.source_index)) : src_sm.mat_name);
        o << "\tMaterial: " << sm_out.material_id << ", \"Material::" << json_escape(mat_name) << "\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t\tMultiLayer: 0\n\t\tProperties70:  {\n\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",1,1,1\n\t\t\tP: \"Diffuse\", \"Vector3D\", \"Vector\", \"\",1,1,1\n\t\t\tP: \"AmbientColor\", \"Color\", \"\", \"A\",0,0,0\n\t\t\tP: \"SpecularColor\", \"Color\", \"\", \"A\",0,0,0\n\t\t\tP: \"Opacity\", \"double\", \"Number\", \"\",1\n\t\t\tP: \"TransparencyFactor\", \"double\", \"Number\", \"\",0\n\t\t}\n\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(sm_out.geom_id) + "," + std::to_string(sm_out.mesh_id));
        conns.push_back("C: \"OO\"," + std::to_string(sm_out.mesh_id) + "," + std::to_string(armature_id));
        conns.push_back("C: \"OO\"," + std::to_string(sm_out.material_id) + "," + std::to_string(sm_out.mesh_id));
        int tr = sm_out.source_index < (int)mat_texture.size() ? mat_texture[sm_out.source_index] : -1;
        if (tr >= 0)
            conns.push_back("C: \"OP\"," + std::to_string(tex_ids[tr]) + "," + std::to_string(sm_out.material_id) + ", \"DiffuseColor\"");
        if (bone_count > 0) {
            o << "\tDeformer: " << sm_out.skin_id << ", \"Deformer::" << sm_out.name << "_skin\", \"Skin\" {\n\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n";
            conns.push_back("C: \"OO\"," + std::to_string(sm_out.skin_id) + "," + std::to_string(sm_out.geom_id));
            for (int bi = 0; bi < bone_count; ++bi) {
                if (sm_out.cluster_indexes[bi].empty()) continue;
                std::string name = req.skeleton->bones[bi].name.empty() ? ("bone_" + std::to_string(bi)) : req.skeleton->bones[bi].name;
                o << "\tDeformer: " << sm_out.cluster_ids[bi] << ", \"SubDeformer::" << json_escape(sm_out.name + "_" + name) << "\", \"Cluster\" {\n\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n\t\tLink_Mode: \"Normalize\"\n\t\tLinkMode: \"Normalize\"\n";
                fbx_array_i(o, "Indexes", sm_out.cluster_indexes[bi]);
                fbx_array(o, "Weights", sm_out.cluster_weights[bi]);
                fbx_array(o, "Transform", fbx_matrix(fbx_root_matrix));
                fbx_array(o, "TransformLink", fbx_matrix(fbx_bind_world[bi]));
                o << "\t}\n";
                conns.push_back("C: \"OO\"," + std::to_string(sm_out.cluster_ids[bi]) + "," + std::to_string(sm_out.skin_id));
                conns.push_back("C: \"OO\"," + std::to_string(bone_ids[bi]) + "," + std::to_string(sm_out.cluster_ids[bi]));
            }
        }
    }

    if (bone_count > 0) {
        o << "\tPose: " << bind_pose_id << ", \"Pose::BindPose\", \"BindPose\" {\n\t\tType: \"BindPose\"\n\t\tVersion: 100\n\t\tNbPoseNodes: " << (bone_count + 1 + (int)fbx_submeshes.size()) << "\n";
        o << "\t\tPoseNode:  {\n\t\t\tNode: " << armature_id << "\n";
        fbx_array(o, "Matrix", fbx_matrix(fbx_root_matrix));
        o << "\t\t}\n";
        for (const FbxSubmesh& sm_out : fbx_submeshes) {
            o << "\t\tPoseNode:  {\n\t\t\tNode: " << sm_out.mesh_id << "\n";
            fbx_array(o, "Matrix", fbx_matrix(fbx_root_matrix));
            o << "\t\t}\n";
        }
        for (int bi = 0; bi < bone_count; ++bi) {
            o << "\t\tPoseNode:  {\n\t\t\tNode: " << bone_ids[bi] << "\n";
            fbx_array(o, "Matrix", fbx_matrix(fbx_bind_world[bi]));
            o << "\t\t}\n";
        }
        o << "\t}\n";
    }
    const int64_t ticks_per_second = 46186158000LL;
    for (size_t ai = 0; ai < sampled.size(); ++ai) {
        int64_t stack = next_id();
        int64_t layer = next_id();
        std::string an = clean_name(sampled[ai].name);
        int64_t stop = sampled[ai].times.empty() ? 0 : (int64_t)std::llround(sampled[ai].times.back() * (double)ticks_per_second);
        o << "\tAnimationStack: " << stack << ", \"AnimStack::" << json_escape(an) << "\", \"\" {\n\t\tProperties70:  {\n\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," << stop << "\n\t\t}\n\t}\n";
        o << "\tAnimationLayer: " << layer << ", \"AnimLayer::BaseLayer\", \"\" {\n\t}\n";
        conns.push_back("C: \"OO\"," + std::to_string(stack) + ",0");
        conns.push_back("C: \"OO\"," + std::to_string(layer) + "," + std::to_string(stack));
        std::vector<int64_t> kt;
        kt.reserve(sampled[ai].times.size());
        for (float t : sampled[ai].times) kt.push_back((int64_t)std::llround(t * (double)ticks_per_second));
        for (int bi = 0; bi < bone_count; ++bi) {
            int64_t node = next_id();
            o << "\tAnimationCurveNode: " << node << ", \"AnimCurveNode::" << json_escape(req.skeleton->bones[bi].name) << "_Lcl Rotation\", \"\" {\n\t\tProperties70:  {\n\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n\t\t}\n\t}\n";
            conns.push_back("C: \"OO\"," + std::to_string(node) + "," + std::to_string(layer));
            conns.push_back("C: \"OP\"," + std::to_string(node) + "," + std::to_string(bone_ids[bi]) + ", \"Lcl Rotation\"");
            std::vector<glm::vec3> eulers = fbx_euler_curve(sampled[ai].rotations[bi]);
            for (int axis = 0; axis < 3; ++axis) {
                int64_t curve = next_id();
                std::vector<double> values;
                values.reserve(eulers.size());
                for (const glm::vec3& e : eulers) {
                    values.push_back(axis == 0 ? e.x : (axis == 1 ? e.y : e.z));
                }
                o << "\tAnimationCurve: " << curve << ", \"AnimCurve::\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4008\n";
                fbx_array_i(o, "KeyTime", kt);
                fbx_array(o, "KeyValueFloat", values);
                std::vector<int64_t> flags(values.size(), 24836);
                std::vector<double> attr(values.size() * 4u, 0.0);
                std::vector<int64_t> ref(values.size(), 1);
                fbx_array_i(o, "KeyAttrFlags", flags);
                fbx_array(o, "KeyAttrDataFloat", attr);
                fbx_array_i(o, "KeyAttrRefCount", ref);
                o << "\t}\n";
                const char* prop = axis == 0 ? "d|X" : (axis == 1 ? "d|Y" : "d|Z");
                conns.push_back("C: \"OP\"," + std::to_string(curve) + "," + std::to_string(node) + ", \"" + prop + "\"");
            }
        }
    }
    o << "}\nConnections:  {\n";
    for (auto& c : conns) o << "\t" << c << "\n";
    o << "}\n";
    std::ofstream f(out, std::ios::binary);
    if (!f) {
        error = "Could not write FBX";
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    if (!f) {
        error = "Could not finish FBX";
        return false;
    }
    return true;
}

}
