#include "scanner.h"

#include <Windows.h>
#include <psapi.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "logger.h"

namespace scanner {

    namespace {

        Stats g_stats{};

        constexpr size_t kChunk = 1u << 20;   // 1 MiB
        constexpr size_t kPage = 4096;
        constexpr size_t kPageBatch = 8192;   // pages described per QueryWorkingSetEx call

        // Raw copy guarded by SEH. VirtualQuery already said the region is
        // committed and readable, but a region can still hide a decommitted or
        // guard page, and ReadProcessMemory is far too slow for gigabytes
        // (~160 MB/s versus several GB/s for memcpy).
        bool raw_copy(const void *src, void *dst, size_t bytes) {
            __try {
                std::memcpy(dst, src, bytes);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool is_scannable(const MEMORY_BASIC_INFORMATION &mbi) {
            if (mbi.State != MEM_COMMIT) return false;
            if (mbi.Type != MEM_PRIVATE) return false;
            if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
            const DWORD p = mbi.Protect & 0xFF;
            return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
                   p == PAGE_EXECUTE_WRITECOPY;
        }

        // Walks every scannable region and hands (base, size) to the callback.
        template <typename Fn>
        void for_each_region(Fn &&fn) {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
            const uintptr_t limit = (uintptr_t)si.lpMaximumApplicationAddress;
            MEMORY_BASIC_INFORMATION mbi{};
            while (addr < limit) {
                if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
                const uintptr_t base = (uintptr_t)mbi.BaseAddress;
                const size_t size = mbi.RegionSize;
                const uintptr_t next = base + size;
                if (next <= addr) break;  // guard against a stuck cursor
                if (is_scannable(mbi)) {
                    g_stats.regions++;
                    g_stats.bytes += size;
                    if (!fn(base, size)) return;
                }
                addr = next;
            }
        }

        // Reads [base, base+size) chunk by chunk. Anything unexpected degrades
        // into a short read rather than an access violation.
        template <typename OnChunk>
        bool for_each_chunk(uintptr_t base, size_t size, std::vector<uint8_t> &buf, OnChunk &&on_chunk) {
            size_t pos = 0;
            while (pos < size) {
                size_t want = size - pos;
                if (want > kChunk) want = kChunk;
                SIZE_T got = want;
                const auto t0 = std::chrono::steady_clock::now();
                if (!raw_copy((const void *)(base + pos), buf.data(), want)) {
                    g_stats.fallbacks++;
                    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(base + pos), buf.data(), want, &got) ||
                        got < 8) {
                        if (want < kChunk) break;
                        pos += kChunk;
                        continue;
                    }
                }
                const auto t1 = std::chrono::steady_clock::now();
                g_stats.chunks++;
                const bool keep_going = on_chunk(base + pos, (size_t)got);
                const auto t2 = std::chrono::steady_clock::now();
                g_stats.copy_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                g_stats.scan_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
                if (!keep_going) return false;
                if (want < kChunk) break;
                pos += kChunk - 7;  // 7 byte overlap so a value never straddles a chunk
            }
            return true;
        }

        // Same walk, but only over pages that are currently resident. Touching a
        // paged-out page costs a disk round trip, and the cold bulk of a running
        // game dwarfs anything worth searching.
        template <typename OnChunk>
        bool for_each_resident_chunk(uintptr_t base, size_t size, std::vector<uint8_t> &buf,
                                     std::vector<PSAPI_WORKING_SET_EX_INFORMATION> &ws,
                                     OnChunk &&on_chunk) {
            size_t off = 0;
            while (off + kPage <= size) {
                size_t pages = (size - off) / kPage;
                if (pages > kPageBatch) pages = kPageBatch;
                for (size_t i = 0; i < pages; ++i) {
                    ws[i].VirtualAddress = (PVOID)(base + off + i * kPage);
                }
                if (QueryWorkingSetEx(GetCurrentProcess(), ws.data(),
                                      (DWORD)(pages * sizeof(PSAPI_WORKING_SET_EX_INFORMATION)))) {
                    size_t i = 0;
                    while (i < pages) {
                        if (!ws[i].VirtualAttributes.Valid) {
                            ++i;
                            continue;
                        }
                        size_t j = i;
                        while (j + 1 < pages && ws[j + 1].VirtualAttributes.Valid) ++j;
                        const uintptr_t run_base = base + off + i * kPage;
                        const size_t run_size = (j - i + 1) * kPage;
                        g_stats.resident_bytes += run_size;
                        if (!for_each_chunk(run_base, run_size, buf, on_chunk)) return false;
                        i = j + 1;
                    }
                }
                off += pages * kPage;
            }
            return true;
        }

    }  // namespace

    std::vector<uintptr_t> find_u64(uint64_t value, size_t max_hits, bool residents_only) {
        std::vector<uintptr_t> hits;
        g_stats = {};
        if (value == 0) return hits;

        std::vector<uint8_t> buf(kChunk);
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> ws(kPageBatch);

        // memchr is vectorised but we still pay per candidate position, so which
        // byte we search for matters enormously. A fixed choice can match tens of
        // thousands of times per MiB and dominate the scan, so sample the first
        // chunk and keep the rarest of the four low bytes.
        uint8_t needle[8];
        std::memcpy(needle, &value, 8);
        uint8_t search_byte = needle[0];
        int search_index = 0;
        bool search_picked = false;

        auto on_chunk = [&](uintptr_t chunk_base, size_t chunk_size) -> bool {
            if (!search_picked && chunk_size >= kChunk) {
                size_t counts[4] = {0, 0, 0, 0};
                for (size_t j = 0; j < chunk_size; ++j) {
                    const uint8_t b = buf[j];
                    if (b == needle[0]) counts[0]++;
                    if (b == needle[1]) counts[1]++;
                    if (b == needle[2]) counts[2]++;
                    if (b == needle[3]) counts[3]++;
                }
                int best = 0;
                for (int k = 1; k < 4; ++k)
                    if (counts[k] < counts[best]) best = k;
                search_index = best;
                search_byte = needle[best];
                search_picked = true;
                LOG("scanner: needle byte histogram %zu/%zu/%zu/%zu per MiB -> searching needle[%d] = 0x%02X",
                    counts[0], counts[1], counts[2], counts[3], search_index, search_byte);
            }

            size_t i = 0;
            while (i < chunk_size) {
                const void *p = std::memchr(buf.data() + i, search_byte, chunk_size - i);
                if (!p) break;
                i = (size_t)((const uint8_t *)p - buf.data());
                if (i >= (size_t)search_index && i - search_index + 8 <= chunk_size) {
                    const size_t start = i - (size_t)search_index;
                    if (std::memcmp(buf.data() + start, needle, 8) == 0) {
                        hits.push_back(chunk_base + start);
                        if (hits.size() >= max_hits) return false;
                    }
                }
                ++i;
            }
            return true;
        };

        for_each_region([&](uintptr_t base, size_t size) -> bool {
            if (residents_only) {
                return for_each_resident_chunk(base, size, buf, ws, on_chunk);
            }
            return for_each_chunk(base, size, buf, on_chunk);
        });

        return hits;
    }

    std::vector<uintptr_t> find_u32(uint32_t value, size_t max_hits, bool residents_only) {
        std::vector<uintptr_t> hits;
        g_stats = {};
        if (value == 0) return hits;

        std::vector<uint8_t> buf(kChunk);
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> ws(kPageBatch);

        uint8_t needle[4];
        std::memcpy(needle, &value, 4);
        uint8_t search_byte = needle[0];
        int search_index = 0;
        bool search_picked = false;

        auto on_chunk = [&](uintptr_t chunk_base, size_t chunk_size) -> bool {
            if (!search_picked && chunk_size >= kChunk) {
                size_t counts[4] = {0, 0, 0, 0};
                for (size_t j = 0; j < chunk_size; ++j) {
                    const uint8_t b = buf[j];
                    if (b == needle[0]) counts[0]++;
                    if (b == needle[1]) counts[1]++;
                    if (b == needle[2]) counts[2]++;
                    if (b == needle[3]) counts[3]++;
                }
                int best = 0;
                for (int k = 1; k < 4; ++k)
                    if (counts[k] < counts[best]) best = k;
                search_index = best;
                search_byte = needle[best];
                search_picked = true;
                LOG("scanner: u32 needle bytes %zu/%zu/%zu/%zu per MiB -> searching byte[%d] = 0x%02X",
                    counts[0], counts[1], counts[2], counts[3], search_index, search_byte);
            }

            size_t i = 0;
            while (i < chunk_size) {
                const void *p = std::memchr(buf.data() + i, search_byte, chunk_size - i);
                if (!p) break;
                i = (size_t)((const uint8_t *)p - buf.data());
                if (i >= (size_t)search_index && i - search_index + 4 <= chunk_size) {
                    const size_t start = i - (size_t)search_index;
                    if (std::memcmp(buf.data() + start, needle, 4) == 0) {
                        hits.push_back(chunk_base + start);
                        if (hits.size() >= max_hits) return false;
                    }
                }
                ++i;
            }
            return true;
        };

        for_each_region([&](uintptr_t base, size_t size) -> bool {
            if (residents_only) {
                return for_each_resident_chunk(base, size, buf, ws, on_chunk);
            }
            return for_each_chunk(base, size, buf, on_chunk);
        });

        return hits;
    }

    std::vector<uintptr_t> find_ptr_in(const std::vector<uintptr_t> &allowed_ranges, uint64_t value,
                                       size_t max_hits) {
        std::vector<uintptr_t> hits;
        std::vector<uint8_t> buf(kChunk);
        for (size_t r = 0; r + 1 < allowed_ranges.size(); r += 2) {
            const uintptr_t base = allowed_ranges[r];
            const size_t size = (size_t)(allowed_ranges[r + 1] - base);
            for_each_chunk(base, size, buf, [&](uintptr_t chunk_base, size_t chunk_size) -> bool {
                for (size_t i = 0; i + 8 <= chunk_size; ++i) {
                    uint64_t full;
                    std::memcpy(&full, buf.data() + i, 8);
                    if (full == value) {
                        hits.push_back(chunk_base + i);
                        if (hits.size() >= max_hits) return false;
                    }
                }
                return true;
            });
        }
        return hits;
    }

    std::vector<uintptr_t> find_ptr_range(uintptr_t lo, uintptr_t hi, size_t max_hits, bool residents_only) {
        std::vector<uintptr_t> hits;
        g_stats = {};
        if (hi <= lo) return hits;

        std::vector<uint8_t> buf(kChunk);
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> ws(kPageBatch);

        uint8_t bytes_lo[8]{};
        uint8_t bytes_hi[8]{};
        std::memcpy(bytes_lo, &lo, 8);
        std::memcpy(bytes_hi, &hi, 8);

        int search_index = -1;
        uint8_t search_byte = 0;

        auto on_chunk = [&](uintptr_t chunk_base, size_t chunk_size) -> bool {
            if (search_index < 0) {
                if (chunk_size < kChunk) return true;  // wait for a full sample
                // A module image is small enough that whole byte positions of the
                // range are constant. Search for the rarest of those; picking a
                // byte that matches every few dozen positions would drown this in
                // full width comparisons.
                size_t best_count = 0;
                for (int k = 7; k >= 0; --k) {
                    if (bytes_lo[k] != bytes_hi[k]) continue;
                    size_t count = 0;
                    for (size_t j = 0; j < chunk_size; ++j)
                        if (buf[j] == bytes_lo[k]) ++count;
                    if (search_index < 0 || count < best_count) {
                        search_index = k;
                        search_byte = bytes_lo[k];
                        best_count = count;
                    }
                }
                if (search_index < 0) {
                    LOG("scanner: no constant byte in the pointer range, falling back to a full walk");
                    search_index = 7;
                    search_byte = bytes_lo[7];
                } else {
                    LOG("scanner: pointer range 0x%llx..0x%llx -> searching byte[%d] = 0x%02X (%zu per MiB)",
                        (unsigned long long)lo, (unsigned long long)hi, search_index, search_byte, best_count);
                }
            }

            size_t i = 0;
            while (i < chunk_size) {
                const void *p = std::memchr(buf.data() + i, search_byte, chunk_size - i);
                if (!p) break;
                i = (size_t)((const uint8_t *)p - buf.data());
                if (i >= (size_t)search_index && i - search_index + 8 <= chunk_size) {
                    const size_t start = i - (size_t)search_index;
                    uintptr_t v = 0;
                    std::memcpy(&v, buf.data() + start, 8);
                    if (v >= lo && v < hi) {
                        hits.push_back(chunk_base + start);
                        if (hits.size() >= max_hits) return false;
                    }
                }
                ++i;
            }
            return true;
        };

        for_each_region([&](uintptr_t base, size_t size) -> bool {
            if (residents_only) return for_each_resident_chunk(base, size, buf, ws, on_chunk);
            return for_each_chunk(base, size, buf, on_chunk);
        });

        return hits;
    }

    Stats last_stats() { return g_stats; }

}  // namespace scanner
