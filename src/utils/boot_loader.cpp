#include "utils/boot_loader.hpp"
#include "utils/logger.hpp"
#include "utils/path_utils.hpp"

namespace pxe {

std::string BootLoader::normalizePath(const std::string& path) {
    std::string result = pathutil::normalizeSlashes(path);
    while (result.rfind("./", 0) == 0) result.erase(0, 2);
    while (!result.empty() && result.front() == '/') result.erase(result.begin());
    return result;
}

std::string BootLoader::getBootfile(const std::string& boot_file) {
    std::string path = normalizePath(boot_file);
    // Strip leading directory segment if any absolute-like prefix slipped through.
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    return path;
}

bool BootLoader::validateBootfile(const std::string& filename) {
    if (filename.empty()) {
        Logger::instance().log(LogLevel::ERR, "Empty bootfile");
        return false;
    }
    if (filename.find("..") != std::string::npos) {
        Logger::instance().log(LogLevel::WARNING, "Invalid bootfile path");
        return false;
    }
    return true;
}

} // namespace pxe
