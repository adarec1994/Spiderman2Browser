#pragma once
#include <string>
#include <vector>

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