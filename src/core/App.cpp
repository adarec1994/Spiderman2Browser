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



#include "AppSkinAndExports.inl"
#include "AppAnimation.inl"
#include "AppSelection.inl"
#include "AppInit.inl"
#include "AppWorld.inl"
#include "AppRun.inl"
