#pragma once
#include <string>

namespace pxe {

class BootLoader {
public:
    static std::string getBootfile(const std::string& boot_file);
    static bool validateBootfile(const std::string& filename);
    static std::string normalizePath(const std::string& path);
};

} // namespace pxe
