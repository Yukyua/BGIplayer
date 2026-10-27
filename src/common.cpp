#include "bgiplayer/common.hpp"
#include "bgiplayer/host_files.hpp"

#include <fstream>
#include <iterator>

namespace bgiplayer {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    return host_files::read(path);
}

}  // namespace bgiplayer
