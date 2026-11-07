// Compile with -I<patched Boxedwine>/source/io. No runtime dependencies.
#include "fsstoredreadcache.h"
#include <cassert>
#include <iostream>
#include <random>
#include <vector>

int main() {
    const uint64_t fileSize = (uint64_t{1} << 33) + 123;
    unsigned calls = 0;
    auto fill = [&](uint64_t at, uint8_t* out, uint32_t count) -> int64_t {
        ++calls;
        assert(at <= fileSize && count <= fileSize - at);
        for (uint32_t i = 0; i < count; ++i) out[i] = uint8_t((at + i) * 37);
        return count;
    };
    FsStoredReadCache cache;
    std::vector<uint8_t> out(512 * 1024);
    for (uint64_t at = 0; at < 1024 * 1024; at += 4096) {
        assert(cache.read(at, fileSize, out.data(), 4096, fill) == 4096);
        for (unsigned i = 0; i < 4096; ++i) assert(out[i] == uint8_t((at + i) * 37));
    }
    assert(calls == 4); // 256 guest pages require four host reads, not 256.
    std::mt19937 rng(42);
    for (unsigned i = 0; i < 500; ++i) {
        uint64_t at = ((uint64_t{1} << 32) + rng()) % fileSize;
        unsigned wanted = rng() % out.size();
        auto count = std::min<uint64_t>(wanted, fileSize - at);
        assert(cache.read(at, fileSize, out.data(), wanted, fill) == int64_t(count));
        for (unsigned j = 0; j < count; ++j) assert(out[j] == uint8_t((at + j) * 37));
    }
    assert(cache.read(fileSize - 5, fileSize, out.data(), 4096, fill) == 5);
    assert(cache.read(fileSize, fileSize, out.data(), 4096, fill) == 0);
    cache.clear();
    auto shortRead = [&](uint64_t at, uint8_t* dst, uint32_t count) {
        return fill(at, dst, std::min(count, uint32_t{17}));
    };
    assert(cache.read(0, fileSize, out.data(), 4096, shortRead) == 4096);
    for (unsigned j = 0; j < 4096; ++j) assert(out[j] == uint8_t(j * 37));
    cache.clear();
    auto fail = [](uint64_t, uint8_t*, uint32_t) -> int64_t { return -5; };
    assert(cache.read(0, fileSize, out.data(), 4096, fail) == -5);
    unsigned before = calls;
    assert(cache.read(0, fileSize, out.data(), 4096, fill) == 4096);
    assert(calls == before + 1);
    auto partial = [&](uint64_t at, uint8_t* dst, uint32_t count) -> int64_t {
        return at >= 256 * 1024 ? -5 : fill(at, dst, count);
    };
    cache.clear();
    assert(cache.read(0, fileSize, out.data(), out.size(), partial) == 256 * 1024);
    std::cout << "Read-ahead checks passed: sequential 64x fewer host reads, random seeks, >4GiB offsets, EOF, short reads, errors, invalidation\n";
}
