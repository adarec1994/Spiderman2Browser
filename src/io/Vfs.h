#pragma once
















#include <string>
#include <vector>
#include <cstdint>

namespace vfs {




bool mount_iso(const std::string& iso_path);


void unmount();


bool mounted();



const std::string& iso_root();


bool is_virtual(const std::string& path);

struct Entry {
    std::string path;     
    bool        is_dir = false;
};



bool exists(const std::string& path);
bool is_directory(const std::string& path);



std::vector<uint8_t> read_file(const std::string& path);



std::vector<std::string> walk_files(const std::string& root);



std::vector<Entry> list_dir(const std::string& dir);

} 
