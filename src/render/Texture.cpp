#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "Texture.h"
#include "Vfs.h"

#include <glad/glad.h>
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <cstdint>

namespace fs = std::filesystem;

static std::unordered_map<std::string, unsigned int> g_cache;


struct DDSPixelFormat {
    uint32_t size, flags, fourCC, rgbBitCount;
    uint32_t rMask, gMask, bMask, aMask;
};
struct DDSHeader {
    uint32_t size, flags, height, width, pitchOrLinearSize;
    uint32_t depth, mipMapCount;
    uint32_t reserved[11];
    DDSPixelFormat pf;
    uint32_t caps[4], reserved2;
};

static unsigned int load_dds(const std::string& path) {
    
    std::vector<uint8_t> file = vfs::read_file(path);
    if (file.size() < 4 + sizeof(DDSHeader)) return 0;
    const uint8_t* fp = file.data();
    size_t fpos = 0;

    uint32_t magic; std::memcpy(&magic, fp, 4); fpos = 4;
    if (magic != 0x20534444) return 0; 

    DDSHeader hdr;
    std::memcpy(&hdr, fp + fpos, sizeof(hdr)); fpos += sizeof(hdr);

    uint32_t w = hdr.width, h = hdr.height;
    uint32_t file_mips = hdr.mipMapCount;          
    uint32_t mips = std::max(1u, file_mips);

    
    uint32_t gl_fmt = 0;
    uint32_t block  = 16;
    char cc[5] = {};
    memcpy(cc, &hdr.pf.fourCC, 4);

    if      (memcmp(cc,"DXT1",4)==0) { gl_fmt = 0x83F1; block = 8; }  
    else if (memcmp(cc,"DXT3",4)==0) { gl_fmt = 0x83F2; }             
    else if (memcmp(cc,"DXT5",4)==0) { gl_fmt = 0x83F3; }             
    else return 0;

    
    size_t data_size = 0;
    uint32_t ww=w, hh=h;
    for (uint32_t m=0; m<mips; ++m) {
        data_size += std::max(1u,(ww+3)/4) * std::max(1u,(hh+3)/4) * block;
        ww=std::max(1u,ww/2); hh=std::max(1u,hh/2);
    }
    std::vector<uint8_t> pixels(data_size);
    if (fpos + data_size > file.size()) data_size = file.size() - fpos; 
    std::memcpy(pixels.data(), fp + fpos, data_size);

    unsigned int tid;
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips>1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    size_t offset = 0;
    ww=w; hh=h;
    for (uint32_t m=0; m<mips; ++m) {
        uint32_t sz = std::max(1u,(ww+3)/4) * std::max(1u,(hh+3)/4) * block;
        glCompressedTexImage2D(GL_TEXTURE_2D, m, gl_fmt, ww, hh, 0, sz, pixels.data()+offset);
        offset += sz;
        ww=std::max(1u,ww/2); hh=std::max(1u,hh/2);
    }
    
    
    
    
    
    
    
    if (file_mips <= 1) {
        glGenerateMipmap(GL_TEXTURE_2D);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(mips - 1));
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return tid;
}


static unsigned int load_stb(const std::string& path) {
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) return 0;
    unsigned int tid;
    glGenTextures(1, &tid);
    glBindTexture(GL_TEXTURE_2D, tid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    return tid;
}

unsigned int load_texture(const std::string& path) {
    auto it = g_cache.find(path);
    if (it != g_cache.end()) return it->second;

    unsigned int tid = 0;
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".dds") tid = load_dds(path);
    if (!tid)          tid = load_stb(path);

    g_cache[path] = tid;
    return tid;
}




static std::unordered_map<std::string, std::string> g_registry;
static std::vector<std::string>                     g_stems_sorted; 

void build_tex_registry(const std::string& root_dir) {
    g_registry.clear();
    g_stems_sorted.clear();
    
    std::vector<std::string> all = vfs::walk_files(root_dir);
    
    for (auto& fpath : all) {
        std::string fn  = fs::path(fpath).filename().string();
        std::string fnl = fn;
        std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
        
        std::string ext2 = fs::path(fnl).extension().string();
        if (ext2 != ".dds" && ext2 != ".tga" && ext2 != ".bmp" &&
            ext2 != ".png" && ext2 != ".jpg" && ext2 != ".jpeg") continue;
        
        if (g_registry.find(fnl) == g_registry.end())
            g_registry[fnl] = fpath;
        
        std::string stem = fs::path(fnl).stem().string();
        if (g_registry.find(stem) == g_registry.end()) {
            g_registry[stem] = fpath;
            g_stems_sorted.push_back(stem);
        }
    }

    
    
    
    
    
    
    
    
    
    
    
    
    for (auto& fpath : all) {
        std::string fnl = fs::path(fpath).filename().string();
        std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
        std::string ext2 = fs::path(fnl).extension().string();
        if (ext2 != ".sg_" && ext2 != ".sd_") continue;

        
        std::string ifl_stem = fs::path(fnl).stem().string();
        if (g_registry.count(ifl_stem)) continue; 

        
        std::vector<uint8_t> ifl_data = vfs::read_file(fpath);
        if (ifl_data.empty()) continue;
        std::string line(reinterpret_cast<const char*>(ifl_data.data()), ifl_data.size());
        
        size_t nl = line.find_first_of("\r\n");
        if (nl != std::string::npos) line.resize(nl);
        
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (line.empty()) continue;

        
        std::transform(line.begin(), line.end(), line.begin(), ::tolower);
        std::string ref_stem = fs::path(line).stem().string();
        if (ref_stem.empty()) continue;

        
        auto it = g_registry.find(ref_stem);
        if (it != g_registry.end()) {
            g_registry[ifl_stem] = it->second;
            g_stems_sorted.push_back(ifl_stem);

            
            
            
            
            static const std::string IFL_SUFFIX = "_ifl";
            if (ifl_stem.size() > IFL_SUFFIX.size() &&
                ifl_stem.substr(ifl_stem.size() - IFL_SUFFIX.size()) == IFL_SUFFIX) {
                std::string base_stem = ifl_stem.substr(0, ifl_stem.size() - IFL_SUFFIX.size());
                if (!base_stem.empty() && g_registry.find(base_stem) == g_registry.end()) {
                    g_registry[base_stem] = it->second;
                    g_stems_sorted.push_back(base_stem);
                }
            }
        }
    }

    
    
    
    
    
    
    
    
    
    
    
    
    
    for (auto& fpath : all) {
        std::string fnl = fs::path(fpath).filename().string();
        std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
        std::string ext2 = fs::path(fnl).extension().string();
        if (ext2 != ".s_b" && ext2 != ".s_t") continue;

        std::string man_stem = fs::path(fnl).stem().string();   
        if (g_registry.count(man_stem)) continue;

        std::vector<uint8_t> data = vfs::read_file(fpath);
        if (data.empty()) continue;
        std::string text(reinterpret_cast<const char*>(data.data()), data.size());

        
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find_first_of("\r\n", pos);
            std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
            pos = (nl == std::string::npos) ? text.size() : nl + 1;
            
            while (!line.empty() && (line.back()==' '||line.back()=='\t'||line.back()=='\r'||line.back()=='\n'))
                line.pop_back();
            if (line.empty()) continue;
            std::transform(line.begin(), line.end(), line.begin(), ::tolower);
            std::string ref_stem = fs::path(line).stem().string();
            if (ref_stem.empty()) continue;
            auto it = g_registry.find(ref_stem);
            if (it != g_registry.end()) {
                g_registry[man_stem] = it->second;
                g_stems_sorted.push_back(man_stem);
                break;   
            }
        }
    }

    std::sort(g_stems_sorted.begin(), g_stems_sorted.end());
}

void register_tex_alias(const std::string& alias, const std::string& target_stem) {
    if (alias.empty() || target_stem.empty()) return;
    std::string a = alias;       std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::string t = target_stem; std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    if (g_registry.count(a)) return;             
    auto it = g_registry.find(t);                
    if (it == g_registry.end()) return;
    g_registry[a] = it->second;
    g_stems_sorted.push_back(a);
    std::sort(g_stems_sorted.begin(), g_stems_sorted.end());
}

static unsigned int registry_lookup(const std::string& hint) {
    if (hint.empty() || g_registry.empty()) return 0;

    
    std::string base = hint;
    {
        static const char* strip_exts[] = { ".tga", ".bmp", ".png", ".jpg", ".jpeg", nullptr };
        std::string lo = hint;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        for (int i = 0; strip_exts[i]; ++i) {
            size_t elen = strlen(strip_exts[i]);
            if (lo.size() > elen && lo.substr(lo.size()-elen) == strip_exts[i]) {
                base = hint.substr(0, hint.size()-elen);
                break;
            }
        }
    }
    std::string basel;
    basel = base;
    std::transform(basel.begin(), basel.end(), basel.begin(), ::tolower);

    std::vector<std::string> tries = {
        basel + ".dds",
        basel,
        [&]{ std::string s=basel; std::transform(s.begin(),s.end(),s.begin(),::toupper); return s+".dds"; }(),
        [&]{ std::string s=base; std::transform(s.begin(),s.end(),s.begin(),::toupper); return s+".DDS"; }(),
    };
    for (auto& t : tries) {
        auto it = g_registry.find(t);
        if (it != g_registry.end()) return load_texture(it->second);
    }

    
    
    
    {
        static const char* suffixes[] = { "_ifl", "_res", "_00", "_01", nullptr };
        for (int i = 0; suffixes[i]; ++i) {
            std::string key = basel + suffixes[i];
            auto it = g_registry.find(key);
            if (it != g_registry.end()) return load_texture(it->second);
        }
    }

    
    
    
    
    
    if (!g_stems_sorted.empty()) {
        std::string cur = basel;
        while (!cur.empty()) {
            std::string prefix = cur + "_";
            auto it = std::lower_bound(g_stems_sorted.begin(), g_stems_sorted.end(), prefix);
            std::string best_numbered, best_named;
            for (; it != g_stems_sorted.end() && it->substr(0, prefix.size()) == prefix; ++it) {
                std::string suffix = it->substr(prefix.size());
                bool all_digits = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(),
                                      [](char c){ return std::isdigit((unsigned char)c); });
                if (all_digits) {
                    if (best_numbered.empty() || *it < best_numbered)
                        best_numbered = *it;
                } else {
                    if (best_named.empty() || it->size() < best_named.size())
                        best_named = *it;
                }
            }
            std::string best = best_numbered.empty() ? best_named : best_numbered;
            if (!best.empty()) {
                auto rit = g_registry.find(best);
                if (rit != g_registry.end()) return load_texture(rit->second);
            }
            
            auto pos = cur.rfind('_');
            if (pos == std::string::npos) break;
            cur = cur.substr(0, pos);
        }
    }
    return 0;
}

unsigned int find_texture(const std::string& hint, const std::string& model_dir) {
    if (hint.empty()) return 0;

    
    std::string base = hint;
    {
        static const char* strip_exts[] = { ".tga", ".bmp", ".png", ".jpg", ".jpeg", nullptr };
        std::string lo = hint;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        for (int i = 0; strip_exts[i]; ++i) {
            size_t elen = strlen(strip_exts[i]);
            if (lo.size() > elen && lo.substr(lo.size()-elen) == strip_exts[i]) {
                base = hint.substr(0, hint.size()-elen);
                break;
            }
        }
    }

    std::vector<std::string> variants = {
        base, base + ".dds",
        [&]{ std::string s=base; std::transform(s.begin(),s.end(),s.begin(),::toupper); return s+".dds"; }(),
        [&]{ std::string s=base; std::transform(s.begin(),s.end(),s.begin(),::tolower); return s+".dds"; }(),
        [&]{ std::string s=base; std::transform(s.begin(),s.end(),s.begin(),::toupper); return s+".DDS"; }(),
    };

    std::vector<std::string> search_dirs = {
        model_dir,
        fs::path(model_dir).parent_path().string(),
        (fs::path(model_dir) / "textures").string(),
    };

    
    
    
    
    
    
    
    static std::unordered_map<std::string, std::unordered_map<std::string,std::string>> s_dir_cache;

    for (auto& dir : search_dirs) {
        auto dit = s_dir_cache.find(dir);
        if (dit == s_dir_cache.end()) {
            std::unordered_map<std::string,std::string> lmap;
            if (vfs::is_directory(dir)) {
                for (auto& entry : vfs::list_dir(dir)) {
                    if (entry.is_dir) continue;
                    std::string fnl = fs::path(entry.path).filename().string();
                    std::transform(fnl.begin(), fnl.end(), fnl.begin(), ::tolower);
                    lmap.emplace(std::move(fnl), entry.path);
                }
            }
            dit = s_dir_cache.emplace(dir, std::move(lmap)).first;
        }
        const auto& lmap = dit->second;
        if (lmap.empty()) continue;
        for (auto& v : variants) {
            std::string vl = v;
            std::transform(vl.begin(), vl.end(), vl.begin(), ::tolower);
            auto it = lmap.find(vl);
            if (it != lmap.end()) return load_texture(it->second);
        }
    }

    
    return registry_lookup(hint);
}

unsigned int find_texture(const std::vector<std::string>& hints, const std::string& model_dir) {
    for (const auto& h : hints) {
        unsigned int tid = find_texture(h, model_dir);
        if (tid) return tid;
    }
    return 0;
}

void get_registry_entries(std::vector<std::pair<std::string,std::string>>& out) {
    out.clear();
    out.reserve(g_stems_sorted.size());
    for (auto& stem : g_stems_sorted) {
        auto it = g_registry.find(stem);
        if (it != g_registry.end())
            out.push_back({stem, it->second});
    }
}

unsigned int find_texture_world(const std::string& hint) {
    return registry_lookup(hint);
}
