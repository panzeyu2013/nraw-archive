#include "archive.h"

#include <cstdint>

namespace nraw {

size_t repack24beToS24le(const void* src, size_t wordCount, void* dst)
{
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < wordCount; ++i) {
        d[3 * i + 0] = s[4 * i + 2];
        d[3 * i + 1] = s[4 * i + 1];
        d[3 * i + 2] = s[4 * i + 0];
    }
    return wordCount * 3;
}

}
