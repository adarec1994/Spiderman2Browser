#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Vfs — virtual filesystem facade.
//
// Goal: let the viewer read an Xbox .iso (xdvdfs / "xiso") image DIRECTLY, while
// behaving EXACTLY like the existing folder code in every other respect.
//
// When an .iso is mounted, any path that begins with the mounted image's file
// path is served from inside the image (virtual paths look like
//   C:/games/SM2.iso/data/packs/GAME/SPIDERMAN/SPIDERMAN.xbx
// so std::filesystem path decomposition — filename()/stem()/parent_path()/
// extension() — keeps working unchanged). Every other path, and everything while
// nothing is mounted, falls through to the real filesystem. So the folder
// workflow is byte-for-byte identical to before.
//
// All file I/O and directory scanning in the app routes through these functions.
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>
#include <cstdint>

namespace vfs {

// Mount an Xbox .iso. Returns false if the file isn't a valid xiso (in which
// case nothing changes and the real filesystem keeps being used). Mounting a new
// image replaces any previous mount.
bool mount_iso(const std::string& iso_path);

// Drop the current mount (back to pure real-filesystem behaviour).
void unmount();

// Is an image currently mounted?
bool mounted();

// The mounted image's file path (the prefix of all virtual paths). Empty if not
// mounted. Callers use this as the "folder" to scan, exactly like a real dir.
const std::string& iso_root();

// True if `path` refers to something inside the mounted image.
bool is_virtual(const std::string& path);

struct Entry {
    std::string path;     // full path (virtual or real)
    bool        is_dir = false;
};

// Mirror of the std::filesystem operations the app uses. Each transparently
// serves from the mounted image when the path is virtual, else the real FS.
bool exists(const std::string& path);
bool is_directory(const std::string& path);

// Read an entire file into memory (empty vector on failure). Replaces the
// per-parser read_file()/ifstream helpers.
std::vector<uint8_t> read_file(const std::string& path);

// Recursive list of every regular file under `root` (full paths). Replaces
// fs::recursive_directory_iterator used for *.xbx / world / texture scans.
std::vector<std::string> walk_files(const std::string& root);

// Immediate children of `dir` (files and subdirectories). Replaces
// fs::directory_iterator used for skeleton/animation pack scans.
std::vector<Entry> list_dir(const std::string& dir);

} // namespace vfs
