#include "hash.h"

#include <bcrypt.h>

namespace vb {

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_HASH_REUSABLE_FLAG);
    if (!NT_SUCCESS(st)) throw Error(Fmt(L"BCryptOpenAlgorithmProvider SHA256: 0x%08lX", static_cast<unsigned long>(st)));
    alg_ = alg;
    BCRYPT_HASH_HANDLE h = nullptr;
    st = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG);
    if (!NT_SUCCESS(st)) throw Error(Fmt(L"BCryptCreateHash: 0x%08lX", static_cast<unsigned long>(st)));
    hash_ = h;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

Hash32 Sha256::Compute(const void* data, size_t len) {
    Hash32 out{};
    auto h = static_cast<BCRYPT_HASH_HANDLE>(hash_);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ULONG n = static_cast<ULONG>(len > 0x40000000u ? 0x40000000u : len);
        NTSTATUS st = BCryptHashData(h, const_cast<PUCHAR>(p), n, 0);
        if (!NT_SUCCESS(st)) throw Error(Fmt(L"BCryptHashData: 0x%08lX", static_cast<unsigned long>(st)));
        p += n;
        len -= n;
    }
    NTSTATUS st = BCryptFinishHash(h, out.data(), static_cast<ULONG>(out.size()), 0);
    if (!NT_SUCCESS(st)) throw Error(Fmt(L"BCryptFinishHash: 0x%08lX", static_cast<unsigned long>(st)));
    return out;
}

std::wstring HashHex(const Hash32& h) {
    std::wstring s;
    for (uint8_t b : h) s += Fmt(L"%02x", b);
    return s;
}

namespace {
#pragma pack(push, 1)
struct FileHeader {
    char magic[8];          // "VXBHASH1"
    uint64_t diskSize;
    uint32_t blockSize;
    uint32_t reserved;
    uint64_t blockCount;
    wchar_t link[128];
    uint8_t checksum[32];   // SHA-256 über alle Block-Hashes
};
#pragma pack(pop)
const char kMagic[8] = {'V', 'X', 'B', 'H', 'A', 'S', 'H', '1'};
}  // namespace

bool HashStore::Load(const std::wstring& path) {
    UniqueHandle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h.Valid()) return false;
    FileHeader hdr{};
    DWORD got = 0;
    if (!ReadFile(h.Get(), &hdr, sizeof(hdr), &got, nullptr) || got != sizeof(hdr)) return false;
    if (memcmp(hdr.magic, kMagic, 8) != 0 || hdr.blockSize == 0) return false;
    uint64_t expected = (hdr.diskSize + hdr.blockSize - 1) / hdr.blockSize;
    if (hdr.blockCount != expected || hdr.blockCount > (1ull << 32)) return false;
    std::vector<Hash32> data(static_cast<size_t>(hdr.blockCount));
    uint64_t total = hdr.blockCount * sizeof(Hash32);
    uint8_t* p = reinterpret_cast<uint8_t*>(data.data());
    while (total > 0) {
        DWORD n = static_cast<DWORD>(total > (64u << 20) ? (64u << 20) : total);
        if (!ReadFile(h.Get(), p, n, &got, nullptr) || got != n) return false;
        p += n;
        total -= n;
    }
    Sha256 sha;
    Hash32 sum = sha.Compute(data.data(), data.size() * sizeof(Hash32));
    if (memcmp(sum.data(), hdr.checksum, 32) != 0) return false;
    diskSize = hdr.diskSize;
    blockSize = hdr.blockSize;
    hdr.link[127] = L'\0';
    link = hdr.link;
    hashes = std::move(data);
    return true;
}

void HashStore::Save(const std::wstring& path) const {
    FileHeader hdr{};
    memcpy(hdr.magic, kMagic, 8);
    hdr.diskSize = diskSize;
    hdr.blockSize = blockSize;
    hdr.blockCount = hashes.size();
    wcsncpy_s(hdr.link, 128, link.c_str(), _TRUNCATE);
    Sha256 sha;
    Hash32 sum = sha.Compute(hashes.data(), hashes.size() * sizeof(Hash32));
    memcpy(hdr.checksum, sum.data(), 32);

    std::wstring tmp = path + L".tmp";
    {
        UniqueHandle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h.Valid()) ThrowWin32(L"Hash-Datei anlegen " + tmp);
        DWORD put = 0;
        if (!WriteFile(h.Get(), &hdr, sizeof(hdr), &put, nullptr) || put != sizeof(hdr)) ThrowWin32(L"Hash-Datei schreiben");
        uint64_t total = hashes.size() * sizeof(Hash32);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(hashes.data());
        while (total > 0) {
            DWORD n = static_cast<DWORD>(total > (64u << 20) ? (64u << 20) : total);
            if (!WriteFile(h.Get(), p, n, &put, nullptr) || put != n) ThrowWin32(L"Hash-Datei schreiben");
            p += n;
            total -= n;
        }
        if (!FlushFileBuffers(h.Get())) ThrowWin32(L"Hash-Datei sichern");
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ThrowWin32(L"Hash-Datei ersetzen " + path);
    }
}

}  // namespace vb
