#include "App.h"
#include "ModelExporter.h"
#include "Texture.h"
#include "WorldParser.h"
#include "Vfs.h"
#include <glad/glad.h>
#include <fstream>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cmath>
#include <cctype>

namespace fs = std::filesystem;
static App* g_app = nullptr;

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
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

static bool is_minion_lizard_model_path(const std::string& path) {
    return strip_instance_suffix(lower_copy(fs::path(path).stem().string())) == "minion_lizard";
}

static bool is_skeleton_dat(const std::string& p) {
    auto buf = vfs::read_file(p);
    return buf.size() >= 4 && *reinterpret_cast<const uint32_t*>(buf.data()) == 0x00B5B58Cu;
}

static std::vector<std::string> ordered_skeleton_candidates(const fs::path& model_path) {
    std::vector<std::string> exact;
    std::vector<std::string> fallback;
    std::string model_stem = lower_copy(model_path.stem().string());
    std::string model_base = strip_instance_suffix(model_stem);

    for (auto& e : vfs::list_dir(model_path.parent_path().string())) {
        if (e.is_dir) continue;
        fs::path p = e.path;
        if (lower_copy(p.extension().string()) != ".dat") continue;
        if (!is_skeleton_dat(e.path)) continue;

        std::string skel_stem = lower_copy(p.stem().string());
        if (skel_stem == model_stem || skel_stem == model_base)
            exact.push_back(p.string());
        else
            fallback.push_back(p.string());
    }

    std::sort(exact.begin(), exact.end());
    std::sort(fallback.begin(), fallback.end());
    exact.insert(exact.end(), fallback.begin(), fallback.end());
    return exact;
}






static std::string resolve_source(const std::string& picked) {
    if (picked.empty()) return "";
    std::error_code ec;
    if (fs::is_directory(picked, ec)) { vfs::unmount(); return picked; }
    if (fs::is_regular_file(picked, ec)) {
        if (vfs::mount_iso(picked)) return picked;   
        vfs::unmount();
        return fs::path(picked).parent_path().string();
    }
    return "";
}

static int vp_x()      { return UI::PANEL_W; }
static int vp_w(int W) { return std::max(W - UI::PANEL_W, 400); }

static glm::quat safe_quat(glm::quat q) {
    float m2 = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    if (!std::isfinite(m2) || m2 <= 1e-12f) return glm::quat(1, 0, 0, 0);
    return q * (1.0f / std::sqrt(m2));
}



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

void App::build_anim_bone_map(const AnimClip& clip) {
    int n_anim = clip.n_bones > 0 ? clip.n_bones : (int)clip.track_count / 3;
    m_anim_bone_map.resize(n_anim, -1);

    int n_skel = m_skeleton ? (int)m_skeleton->bones.size() : 0;
    for (int i = 0; i < n_anim; ++i) {
        if (i < (int)clip.bone_indices.size()) {
            int si = clip.bone_indices[i];
            m_anim_bone_map[i] = (si < n_skel) ? si : -1;
        } else {
            m_anim_bone_map[i] = (i < n_skel) ? i : -1;
        }
    }
}





void App::ensure_global_root_ref() {
    if (m_anim_global_ref_set) return;
    m_anim_global_ref_set = true;
    m_anim_root_ref = glm::quat(1, 0, 0, 0);
    if (m_full_rest_pose.empty() || m_anim_clips.empty()) return;

    auto upper = [](std::string s){ for (auto& c : s) if (c >= 'a' && c <= 'z') c -= 32; return s; };
    int idle = -1;
    for (int i = 0; i < (int)m_anim_clips.size(); ++i)
        if (upper(m_anim_clips[i].name).find("IDL") != std::string::npos) { idle = i; break; }
    if (idle < 0) idle = 0; 

    AnimClip& ic = m_anim_clips[idle];
    if (!ic.loaded) parse_animation(ic, m_skel_meta);
    ic.rest_pose.resize(ic.n_bones);
    for (int ai = 0; ai < ic.n_bones; ++ai) {
        int si = (ai < (int)ic.bone_indices.size()) ? ic.bone_indices[ai] : ai;
        ic.rest_pose[ai] = (si >= 0 && si < (int)m_full_rest_pose.size())
                         ? m_full_rest_pose[si] : glm::quat(1, 0, 0, 0);
    }
    ic.frames_decoded = false;
    ic.cached_frames.clear();
    auto p0 = ic.sample_pose(0.f);
    if (!p0.empty()) m_anim_root_ref = safe_quat(p0[0].q);
}

void App::select_animation(int idx) {
    if (idx < 0 || idx >= (int)m_anim_clips.size()) {
        m_anim_sel  = -1;
        m_anim_play = false;
        if (m_has_bones) {
            for (int i = 0; i < N_BONES; ++i) m_cur_pose[i] = m_bind_pose[i];
            upload_skinning();
        }
        return;
    }

    m_anim_sel  = idx;
    m_anim_time = 0.f;
    m_anim_play = true;
    m_ui_state.anim_time = 0.f;
    m_ui_state.anim_playing = true;

    AnimClip& clip = m_anim_clips[idx];
    if (!clip.loaded) parse_animation(clip, m_skel_meta);
    m_ui_state.anim_dur = clip.duration > 0 ? clip.duration
                        : (clip.fps > 0 && clip.frame_count > 1
                           ? (float)(clip.frame_count - 1) / clip.fps : 0.f);
    if (!m_full_rest_pose.empty()) {
        clip.rest_pose.resize(clip.n_bones);
        for (int ai = 0; ai < clip.n_bones; ++ai) {
            int si = (ai < (int)clip.bone_indices.size()) ? clip.bone_indices[ai] : ai;
            clip.rest_pose[ai] = (si >= 0 && si < (int)m_full_rest_pose.size())
                               ? m_full_rest_pose[si]
                               : glm::quat(1,0,0,0);
        }
        clip.frames_decoded = false;  
        clip.cached_frames.clear();
        clip.cached_root_orientations.clear();
    }

    if (m_skeleton) build_anim_bone_map(clip);

    
    
    ensure_global_root_ref();

    apply_animation_pose(0.f);
    m_last_frame = glfwGetTime();
}

void App::apply_animation_pose(float t) {
    if (!m_has_bones) return;
    if (m_anim_sel < 0 || m_anim_sel >= (int)m_anim_clips.size()) return;

    const AnimClip& clip = m_anim_clips[m_anim_sel];
    if (!clip.loaded || !m_skeleton) return;

    for (int i = 0; i < N_BONES; ++i) m_cur_pose[i] = m_bind_pose[i];

    auto poses = clip.sample_pose(t);
    int n_anim = (int)poses.size();
    int n_skel = (int)m_skeleton->bones.size();

    
    
    std::vector<glm::mat4> local_bind(n_skel, glm::mat4(1.0f));
    for (int i = 0; i < n_skel && i < N_BONES; ++i) {
        int par = m_skeleton->bones[i].parent;
        local_bind[i] = (par >= 0 && par < N_BONES)
                      ? glm::inverse(m_bind_pose[par]) * m_bind_pose[i]
                      : m_bind_pose[i];
    }

    std::vector<BonePose> pose_by_skel(n_skel);
    std::vector<bool> has_pose(n_skel, false);
    for (int ai = 0; ai < n_anim; ++ai) {
        int si = (ai < (int)m_anim_bone_map.size()) ? m_anim_bone_map[ai] : ai;
        if (si < 0 || si >= n_skel || si >= N_BONES) continue;
        pose_by_skel[si] = poses[ai];
        has_pose[si] = true;
    }

    for (int i = 0; i < n_skel && i < N_BONES; ++i) {
        int par = m_skeleton->bones[i].parent;
        glm::vec4 local_t = local_bind[i][3];
        glm::quat bind_q = safe_quat(glm::quat_cast(glm::mat3(local_bind[i])));
        glm::quat local_q = bind_q;

        if (has_pose[i]) {
            BonePose bp = pose_by_skel[i];
            float q_mag2 = bp.q.x*bp.q.x + bp.q.y*bp.q.y + bp.q.z*bp.q.z + bp.q.w*bp.q.w;
            if (std::isfinite(q_mag2) && q_mag2 >= 1e-8f) {
                glm::quat anim_q = safe_quat(bp.q);
                if (par < 0) {
                    
                    
                    
                    
                    
                    
                    local_q = safe_quat((bind_q * glm::inverse(safe_quat(m_anim_root_ref))) * anim_q);
                } else {
                    
                    
                    
                    
                    local_q = anim_q;
                }
            }
        }

        if (m_minion_lizard_model) {
            float anim_weight = 1.0f;
            switch (i) {
                case 10: case 22:
                    anim_weight = 0.80f;
                    break;
                case 11: case 12: case 13: case 14:
                case 15: case 16: case 17: case 18:
                case 23: case 24: case 25: case 26:
                case 27: case 28: case 29: case 30:
                    anim_weight = 0.45f;
                    break;
                case 32: case 38:
                    anim_weight = 0.70f;
                    break;
                case 33: case 34: case 35: case 36:
                case 39: case 40: case 41: case 42:
                    anim_weight = 0.62f;
                    break;
                default:
                    break;
            }
            if (anim_weight < 1.0f) {
                if (glm::dot(bind_q, local_q) < 0.0f) local_q = -local_q;
                local_q = safe_quat(glm::slerp(bind_q, local_q, anim_weight));
            }
        }

        glm::mat4 local_mat = glm::mat4_cast(local_q);
        local_mat[3] = local_t;

        if (par >= 0 && par < N_BONES)
            m_cur_pose[i] = m_cur_pose[par] * local_mat;
        else
            m_cur_pose[i] = local_mat;
    }

    upload_skinning();
    if (m_skeleton) {
        for (int i = 0; i < N_BONES && i < (int)m_skeleton->bones.size(); ++i)
            m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);
        rebuild_skel_gpu();
    }
}

void App::tick_animation(double now) {
    if (!m_anim_play || m_anim_sel < 0) {
        m_last_frame = now;
        return;
    }

    double dt = now - m_last_frame;
    m_last_frame = now;
    if (dt > 0.1) dt = 0.1; 

    const AnimClip& clip = m_anim_clips[m_anim_sel];
    float dur = clip.duration > 0 ? clip.duration
              : (clip.fps > 0 && clip.frame_count > 1
                 ? (float)(clip.frame_count - 1) / clip.fps : 1.f);

    m_anim_time += (float)dt;
    if (dur > 0.0f) {
        if (clip.looping) {
            m_anim_time = std::fmod(m_anim_time, dur);
            if (m_anim_time < 0.0f) m_anim_time += dur;
        } else if (m_anim_time >= dur) {
            m_anim_time = dur;
            m_anim_play = false;
            m_ui_state.anim_playing = false;
        }
    }
    m_ui_state.anim_time  = m_anim_time;
    m_ui_state.anim_dur   = dur;

    apply_animation_pose(m_anim_time);
}



int App::pick_bone(double mx, double my) {
    if (!m_skeleton || !m_gpu_model) return -1;
    int w = vp_w(m_w), h = m_h;
    glm::vec3 ro = m_cam.eye();
    glm::vec3 rd = m_cam.ray_dir((float)mx,(float)my, vp_x(), w, h);
    float s = m_gpu_model->scale;
    glm::vec3 c = m_gpu_model->center;
    float pick_r = s * 0.03f;

    float best_t = 1e9f;
    int   best_i = -1;
    for (int i = 0; i < (int)m_skeleton->bones.size(); ++i) {
        glm::vec3 bp = (m_skeleton->bones[i].world_pos - c) / s;
        glm::vec3 oc = ro - bp;
        float b    = glm::dot(oc,rd);
        float r    = pick_r/s;
        float disc = b*b - glm::dot(oc,oc) + r*r;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t > 0 && t < best_t) { best_t=t; best_i=i; }
    }
    return best_i;
}


static bool ray_tri(glm::vec3 ro, glm::vec3 rd,
                    glm::vec3 v0, glm::vec3 v1, glm::vec3 v2, float& t) {
    const float EPS = 1e-7f;
    glm::vec3 e1=v1-v0, e2=v2-v0, h=glm::cross(rd,e2);
    float a = glm::dot(e1,h);
    if (fabsf(a)<EPS) return false;
    float f = 1.f/a;
    glm::vec3 s=ro-v0;
    float u = f*glm::dot(s,h);
    if (u<0.f||u>1.f) return false;
    glm::vec3 q=glm::cross(s,e1);
    float v = f*glm::dot(rd,q);
    if (v<0.f||u+v>1.f) return false;
    t = f*glm::dot(e2,q);
    return t>EPS;
}


static std::vector<uint32_t> build_tris_for_pick(
    const std::vector<uint16_t>& raw, uint32_t vc, int method_sel)
{
    std::vector<uint32_t> tris;
    if (!raw.empty()) {
        if (method_sel==1) { 
            for (size_t i=0;i+2<raw.size();i+=3) {
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2];
                if (a<vc&&b<vc&&c<vc&&a!=b&&b!=c&&a!=c){tris.push_back(a);tris.push_back(b);tris.push_back(c);}
            }
        } else if (method_sel==2) { 
            for (size_t i=0;i+3<raw.size();i+=4) {
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2],d=raw[i+3];
                if (a<vc&&b<vc&&c<vc&&d<vc){
                    tris.push_back(a);tris.push_back(b);tris.push_back(c);
                    tris.push_back(a);tris.push_back(c);tris.push_back(d);}
            }
        } else if (method_sel==3) { 
            if (!raw.empty()&&raw[0]<vc)
                for (size_t i=1;i+1<raw.size();++i){
                    uint32_t b=raw[i],c=raw[i+1];
                    if (b<vc&&c<vc&&raw[0]!=b&&b!=c&&raw[0]!=c){tris.push_back(raw[0]);tris.push_back(b);tris.push_back(c);}
                }
        } else { 
            int p=0;
            for (size_t i=0;i+2<raw.size();++i){
                uint32_t a=raw[i],b=raw[i+1],c=raw[i+2];
                if (a>=vc||b>=vc||c>=vc){p^=1;continue;}
                if (a==b||b==c||a==c){p^=1;continue;}
                if (p){tris.push_back(a);tris.push_back(c);tris.push_back(b);}
                else  {tris.push_back(a);tris.push_back(b);tris.push_back(c);}
                p^=1;
            }
        }
    } else { 
        int p=0;
        for (uint32_t i=0;i+2<vc;++i){
            if (p){tris.push_back(i);tris.push_back(i+2);tris.push_back(i+1);}
            else  {tris.push_back(i);tris.push_back(i+1);tris.push_back(i+2);}
            p^=1;
        }
    }
    return tris;
}

void App::try_select(double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;

    
    if (m_gpu_model && !m_cached_raw.empty()) {
        int w = vp_w(m_w), h = m_h;
        glm::vec3 ro = m_cam.eye();
        glm::vec3 rd = m_cam.ray_dir((float)mx,(float)my, vp_x(), w, h);

        
        glm::mat4 T    = glm::translate(glm::mat4(1), -m_gpu_model->center);
        glm::mat4 Ry   = glm::rotate(glm::mat4(1), m_model_rot_y, glm::vec3(0,1,0));
        glm::mat4 S    = glm::scale(glm::mat4(1), glm::vec3(1.f/m_gpu_model->scale));
        glm::mat4 Minv = glm::inverse(S * Ry * T);

        
        glm::vec3 lo = glm::vec3(Minv * glm::vec4(ro, 1.f));
        glm::vec3 ld = glm::normalize(glm::vec3(Minv * glm::vec4(rd, 0.f)));

        float best_t  = 1e30f;
        int   best_sm = -1;

        for (int si=0; si<(int)m_cached_raw.size(); ++si) {
            auto& rm = m_cached_raw[si];
            if (rm.positions.empty()) continue;
            int method = (si<(int)m_ui_state.submeshes.size()) ?
                          m_ui_state.submeshes[si].method_sel : 0;
            auto tris = build_tris_for_pick(rm.raw, rm.vc, method);
            for (size_t ti=0; ti+2<tris.size(); ti+=3) {
                float t;
                if (ray_tri(lo,ld, rm.positions[tris[ti]],
                                   rm.positions[tris[ti+1]],
                                   rm.positions[tris[ti+2]], t) && t<best_t) {
                    best_t=t; best_sm=si;
                }
            }
        }

        if (best_sm >= 0) {
            m_ui_state.sel_submesh = best_sm;
            m_renderer.sel_submesh = best_sm;
            
            if (m_gpu_model && best_sm < (int)m_gpu_model->meshes.size()) {
                m_ui_state.preview_tex_id   = m_gpu_model->meshes[best_sm].tex_id;
                m_ui_state.preview_tex_name = best_sm < (int)m_ui_state.submeshes.size()
                    ? m_ui_state.submeshes[best_sm].tex_candidates.empty()
                        ? m_ui_state.submeshes[best_sm].mat_name
                        : m_ui_state.submeshes[best_sm].tex_candidates[0]
                    : "";
            }
            return;
        }
    }

    
    int b = pick_bone(mx,my);
    m_sel_bone = b;
    m_renderer.sel_bone = b;
}



void App::begin_rotate(double mx) {
    if (!m_gpu_model) return;
    m_rot_mode    = true;
    m_rot_start_x = mx;
    m_rot_accum   = 0.f;
    m_rot_axis    = {0,1,0};
    if (m_sel_bone >= 0 && m_has_bones) m_cur_pose_backup = m_cur_pose;
}

void App::update_rotate(double mx) {
    if (!m_rot_mode) return;
    float angle = (float)(mx - m_rot_start_x) / (float)m_w * glm::two_pi<float>();
    float delta = angle - m_rot_accum;
    m_rot_accum  = angle;
    glm::mat4 R  = glm::rotate(glm::mat4(1), delta, m_rot_axis);

    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        glm::vec3 pivot = glm::vec3(m_cur_pose[m_sel_bone][3]);
        int nb = (int)m_skeleton->bones.size();
        std::vector<bool> aff(nb, false);
        aff[m_sel_bone] = true;
        for (int i = 0; i < nb; ++i)
            if (m_skeleton->bones[i].parent >= 0 && aff[m_skeleton->bones[i].parent])
                aff[i] = true;

        for (int i = 0; i < nb; ++i) {
            if (!aff[i]) continue;
            glm::mat4& M = m_cur_pose[i];
            glm::vec3 t  = glm::vec3(M[3]);
            M    = R * M;
            M[3] = glm::vec4(glm::vec3(R * glm::vec4(t - pivot, 0.f)) + pivot, 1.f);
        }
        for (int i = 0; i < nb; ++i)
            if (aff[i]) m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);

        upload_skinning();
        rebuild_skel_gpu();
    } else {
        m_model_rot_y += delta;
        m_renderer.model_rot_y = m_model_rot_y;
    }
}

void App::confirm_rotate() { m_rot_mode = false; }

void App::cancel_rotate() {
    if (m_sel_bone >= 0 && m_has_bones && m_skeleton) {
        m_cur_pose = m_cur_pose_backup;
        for (int i = 0; i < (int)m_skeleton->bones.size(); ++i)
            m_skeleton->bones[i].world_pos = glm::vec3(m_cur_pose[i][3]);
        upload_skinning();
        rebuild_skel_gpu();
    } else {
        m_model_rot_y -= m_rot_accum;
        m_renderer.model_rot_y = m_model_rot_y;
    }
    m_rot_mode = false;
}

void App::rebuild_skel_gpu() {
    if (m_gpu_skel && m_skeleton) m_gpu_skel->build(*m_skeleton);
}



void App::cb_key(GLFWwindow* w, int key, int, int action, int) {
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    if (action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) {
            if (g_app->m_rot_mode) {
                g_app->cancel_rotate();
            } else {
                g_app->m_ui_state.sel_submesh = -1;
                g_app->m_renderer.sel_submesh = -1;
                g_app->m_ui_state.preview_tex_id = 0;
                g_app->m_ui_state.preview_tex_name.clear();
                g_app->m_sel_bone = -1;
                g_app->m_renderer.sel_bone = -1;
            }
        }
        if (key == GLFW_KEY_R && !g_app->m_rot_mode) {
            double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
            g_app->begin_rotate(mx);
        }
        if (g_app->m_rot_mode) {
            if (key==GLFW_KEY_X) g_app->m_rot_axis={1,0,0};
            if (key==GLFW_KEY_Y) g_app->m_rot_axis={0,1,0};
            if (key==GLFW_KEY_Z) g_app->m_rot_axis={0,0,1};
        }
        
        if (key == GLFW_KEY_SPACE) {
            if (g_app->m_anim_sel >= 0) {
                if (!g_app->m_anim_play &&
                    g_app->m_anim_sel < (int)g_app->m_anim_clips.size()) {
                    const AnimClip& clip = g_app->m_anim_clips[g_app->m_anim_sel];
                    float dur = clip.duration > 0 ? clip.duration
                              : (clip.fps > 0 && clip.frame_count > 1
                                 ? (float)(clip.frame_count - 1) / clip.fps : 1.f);
                    if (g_app->m_anim_time >= dur) g_app->m_anim_time = 0.f;
                }
                g_app->m_anim_play          = !g_app->m_anim_play;
                g_app->m_ui_state.anim_playing = g_app->m_anim_play;
                g_app->m_ui_state.anim_time = g_app->m_anim_time;
                g_app->m_last_frame         = glfwGetTime();
                if (!g_app->m_anim_play) {
                    
                    g_app->apply_animation_pose(g_app->m_anim_time);
                }
            }
        }
    }
    if ((action==GLFW_PRESS||action==GLFW_REPEAT) && key==GLFW_KEY_ENTER && g_app->m_rot_mode)
        g_app->confirm_rotate();
}

void App::cb_mouse_btn(GLFWwindow*, int btn, int action, int) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
    bool in_vp = mx >= vp_x();

    if (action == GLFW_PRESS) {
        if (btn == GLFW_MOUSE_BUTTON_LEFT) {
            if (g_app->m_rot_mode) g_app->confirm_rotate();
            else if (in_vp) {
                if (!g_app->m_world_mode) g_app->try_select(mx,my);
                g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
            }
        }
        if (btn == GLFW_MOUSE_BUTTON_RIGHT && in_vp) {
            if (g_app->m_rot_mode) { g_app->cancel_rotate(); }
            else if (g_app->m_world_mode && g_app->m_cam.fly) {
                
                g_app->m_fly_look = true;
                g_app->m_lx = mx; g_app->m_ly = my;
                glfwSetInputMode(g_app->m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            } else {
                g_app->m_drag_r=true; g_app->m_lx=mx; g_app->m_ly=my;
            }
        }
        if (btn == GLFW_MOUSE_BUTTON_MIDDLE && in_vp) {
            g_app->m_drag_l=true; g_app->m_lx=mx; g_app->m_ly=my;
        }
    } else {
        if (btn==GLFW_MOUSE_BUTTON_LEFT||btn==GLFW_MOUSE_BUTTON_MIDDLE) g_app->m_drag_l=false;
        if (btn==GLFW_MOUSE_BUTTON_RIGHT) {
            g_app->m_drag_r=false;
            if (g_app->m_fly_look) {
                g_app->m_fly_look = false;
                glfwSetInputMode(g_app->m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }
}

void App::cb_cursor(GLFWwindow*, double mx, double my) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double dx=mx-g_app->m_lx, dy=my-g_app->m_ly;
    g_app->m_lx=mx; g_app->m_ly=my;
    if (g_app->m_rot_mode) { g_app->update_rotate(mx); return; }
    if (g_app->m_fly_look) { g_app->m_cam.fly_look((float)dx,(float)dy); return; }
    if (g_app->m_drag_l)   g_app->m_cam.orbit((float)dx,(float)dy);
    if (g_app->m_drag_r)   g_app->m_cam.do_pan((float)dx,(float)dy);
}

void App::cb_scroll(GLFWwindow*, double, double dy) {
    if (ImGui::GetIO().WantCaptureMouse) return;
    double mx,my; glfwGetCursorPos(g_app->m_window,&mx,&my);
    if (mx>=vp_x()) g_app->m_cam.zoom((float)dy);
}

void App::cb_resize(GLFWwindow*, int w, int h) {
    g_app->m_w=w; g_app->m_h=h; glViewport(0,0,w,h);
}



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
