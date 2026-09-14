#include <cstdio>
#include <fstream>
#include <string>

namespace {

bool check(bool condition, const char* expression) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "check failed: %s\n", expression);
    return false;
}

bool contains_line(const std::string& config, const std::string& line) {
    return config.find(line + "\n") != std::string::npos || config.ends_with(line);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: fatfs_config_test <sdkconfig>\n");
        return 1;
    }

    std::ifstream file(argv[1]);
    const std::string config((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return check(file.good() || file.eof(), "sdkconfig can be read") &&
                   check(contains_line(config, "CONFIG_FATFS_LFN_HEAP=y"),
                         "FAT long filenames use a heap buffer") &&
                   check(!contains_line(config, "CONFIG_FATFS_LFN_NONE=y"),
                         "FAT short-name-only mode is disabled") &&
                   check(contains_line(config, "CONFIG_FATFS_API_ENCODING_UTF_8=y"),
                         "FAT filenames are returned as UTF-8")
               ? 0
               : 1;
}
