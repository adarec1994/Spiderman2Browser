#pragma once
#include <string>
#include <vector>

// Diagnostic: absolute path of the most recently resolved texture (set by find_texture).
extern std::string g_diag_last_tex_path;

// Load a DDS (DXT1/DXT5) or any stb_image-supported texture.
// Returns OpenGL texture ID, or 0 on failure.
unsigned int load_texture(const std::string& path);

// Search for hint (with DDS extension variants) in model_dir, its parent, and model_dir/textures.
// Falls back to the global registry if local search fails.
// Returns GL texture ID or 0.
unsigned int find_texture(const std::string& hint, const std::string& model_dir);

// Try each candidate in order; return the first that loads successfully.
unsigned int find_texture(const std::vector<std::string>& hints, const std::string& model_dir);

// Build a global filename registry by recursively scanning root_dir.
// Call once from App::scan_folder(). Enables cross-pack texture resolution.
void build_tex_registry(const std::string& root_dir);

// Returns all indexed stems + their file paths, sorted alphabetically.
void get_registry_entries(std::vector<std::pair<std::string,std::string>>& out);

// Registry-only lookup (skips local directory scan).
// Use this for world/environment models where the global registry is always sufficient.
unsigned int find_texture_world(const std::string& hint);

// Register a runtime alias: looking up `alias` (lowercase, no ext) will resolve to
// the same file as `target_stem` (if the target is in the registry). Used to map a
// building master material (s_blg_blgmaster) to a cell's real wall texture. Only
// sets the alias if not already present (first cell wins) and the target resolves.
void register_tex_alias(const std::string& alias, const std::string& target_stem);