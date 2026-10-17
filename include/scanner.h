#ifndef ORTHOCAM_SCANNER_H
#define ORTHOCAM_SCANNER_H

#include <cstdint>
#include <vector>

namespace scanner {

    struct Stats {
        size_t regions = 0;
        size_t bytes = 0;
        size_t resident_bytes = 0;  // of bytes, how much was actually in the working set
        size_t chunks = 0;
        size_t fallbacks = 0;   // chunks where the fast copy faulted
        double copy_ms = 0.0;   // time spent pulling bytes
        double scan_ms = 0.0;   // time spent searching them
    };

    // Scans committed, private, writable memory for an 8 byte little endian value.
    // This is the heap counterpart of mem::FindSig, which only walks the module image.
    //
    // residents_only walks just the pages currently in the working set, which is
    // what makes this fast: the cold bulk of the process (staging buffers, data
    // for levels that are not loaded) dominates the byte count but costs disk I/O
    // to touch, while the thing we are hunting is used every frame.
    std::vector<uintptr_t> find_u64(uint64_t value, size_t max_hits, bool residents_only = true);

    // Same walk for a 4 byte pattern. Used to hunt a specific float, which is far
    // rarer than the 8 byte anchor that turned out to match everything.
    std::vector<uintptr_t> find_u32(uint32_t value, size_t max_hits, bool residents_only = true);

    // Same walk, but matching any 8 byte value inside [lo, hi). A COM object
    // keeps its vtable pointer in its first word and every vtable lives in the
    // owning module's image, so scanning for pointers into a known module is how
    // the D3D12 hook finds the live device.
    std::vector<uintptr_t> find_ptr_range(uintptr_t lo, uintptr_t hi, size_t max_hits,
                                          bool residents_only = true);

    // Same walk, but reporting every address where `predicate` returns true.
    // The predicate gets (address, offset inside chunk) and is called for every
    // 8 byte aligned-ish position, so keep it cheap.
    std::vector<uintptr_t> find_ptr_in(const std::vector<uintptr_t> &allowed_ranges, uint64_t value,
                                       size_t max_hits);

    Stats last_stats();

}  // namespace scanner

#endif
