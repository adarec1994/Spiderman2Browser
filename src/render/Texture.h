#pragma once
#include <string>
#include <vector>


unsigned int load_texture(const std::string& path);




unsigned int find_texture(const std::string& hint, const std::string& model_dir);


unsigned int find_texture(const std::vector<std::string>& hints, const std::string& model_dir);


std::string resolve_texture_path(const std::string& hint, const std::string& model_dir);


std::string resolve_texture_path(const std::vector<std::string>& hints, const std::string& model_dir);



void build_tex_registry(const std::string& root_dir);


void get_registry_entries(std::vector<std::pair<std::string,std::string>>& out);



unsigned int find_texture_world(const std::string& hint);





void register_tex_alias(const std::string& alias, const std::string& target_stem);
