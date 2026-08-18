#pragma once
#include <map>
#include <string>

// flat string map persisted as key=value lines at <data_dir>/creds/<name>, 0600
std::map<std::string, std::string> creds_load(const std::string &name);
void creds_save(const std::string &name, const std::map<std::string, std::string> &kv);
