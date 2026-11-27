// SHA-256 über Windows CNG (BCrypt) und die Block-Hash-Datei einer Generation.
#pragma once

#include "common.h"

#include <array>

namespace vb {

using Hash32 = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Hash32 Compute(const void* data, size_t len);
private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};

// Pro 1-MiB-Block der Platte der SHA-256 des Inhalts, den das neueste Kettenglied an dieser Stelle hat.
// Alles Null = unbekannt (Block wurde nie geschrieben und ist im Image leer).
struct HashStore {
    static constexpr uint32_t kBlockSize = 1u << 20;

    uint64_t diskSize = 0;
    uint32_t blockSize = kBlockSize;
    std::wstring link;              // Dateiname des Kettenglieds, das diese Hashes beschreibt
    std::vector<Hash32> hashes;

    void Init(uint64_t size) {
        diskSize = size;
        blockSize = kBlockSize;
        hashes.assign(static_cast<size_t>((size + blockSize - 1) / blockSize), Hash32{});
    }
    uint64_t BlockCount() const { return hashes.size(); }
    static bool Known(const Hash32& h) {
        for (uint8_t b : h) if (b) return true;
        return false;
    }

    // false, wenn die Datei fehlt oder beschädigt ist.
    bool Load(const std::wstring& path);
    void Save(const std::wstring& path) const;   // atomar über .tmp und MoveFileEx
};

std::wstring HashHex(const Hash32& h);

}  // namespace vb
