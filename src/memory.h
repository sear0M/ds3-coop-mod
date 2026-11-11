#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

// Safe reads of game memory. Every pointer we follow is checked with VirtualQuery first,
// so a wrong offset gives "not found" instead of crashing the game.
namespace mem {

inline bool Readable(const void* p, size_t size) {
    if (!p || size == 0) return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    auto addr = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = addr + size;
    if (end < addr) return false;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & kReadable)) return false;
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

template <class T>
bool Read(uintptr_t addr, T& out) {
    if (!Readable(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return true;
}

inline bool Writable(const void* p, size_t size) {
    if (!p || size == 0) return false;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    auto addr = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = addr + size;
    if (end < addr) return false;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & kWritable)) return false;
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

template <class T>
bool Write(uintptr_t addr, const T& value) {
    if (!Writable(reinterpret_cast<const void*>(addr), sizeof(T))) return false;
    std::memcpy(reinterpret_cast<void*>(addr), &value, sizeof(T));
    return true;
}

}  // namespace mem
