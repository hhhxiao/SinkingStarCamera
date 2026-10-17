#ifndef ORTHOCAM_MISC_H
#define ORTHOCAM_MISC_H

#include <filesystem>

namespace misc {
    // Directory the mod dll lives in (the game folder when installed as winmm.dll).
    std::filesystem::path thisDllDirectory();
}  // namespace misc

#endif
