#pragma once

// Replacing virtual functions of SDK objects by patching their vtable slots.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace nhl3on3 {

// Byte offset of a virtual function's slot in its vtable, from a pointer to
// member function.
template <typename Fn>
ptrdiff_t VtableOffset(Fn fn) {
#if defined(_MSC_VER) || defined(_WIN32)
  // MSVC ABI: the pointer is a thunk `mov rax,[rcx]; jmp [rax+disp]`, or with
  // clang-cl `mov rax,[rcx]; mov rax,[rax+disp]; jmp rax`.
  const uint8_t* p;
  std::memcpy(&p, &fn, sizeof(p));
  while (p[0] == 0xE9) {
    int32_t rel;
    std::memcpy(&rel, p + 1, 4);
    p += 5 + rel;
  }
  if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x01 && p[3] == 0xFF) {
    if (p[4] == 0x60) {
      return int8_t(p[5]);
    }
    if (p[4] == 0xA0) {
      int32_t disp;
      std::memcpy(&disp, p + 5, 4);
      return disp;
    }
    if (p[4] == 0x20) {
      return 0;
    }
  }
  if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x01 && p[3] == 0x48 && p[4] == 0x8B) {
    if (p[5] == 0x40) {
      return int8_t(p[6]);
    }
    if (p[5] == 0x80) {
      int32_t disp;
      std::memcpy(&disp, p + 6, 4);
      return disp;
    }
    if (p[5] == 0x00) {
      return 0;
    }
  }
  return -1;
#else
  // Itanium ABI: {1 + vtable offset, this adjustment}.
  struct {
    uintptr_t ptr;
    ptrdiff_t adj;
  } raw;
  static_assert(sizeof(raw) == sizeof(fn));
  std::memcpy(&raw, &fn, sizeof(raw));
  return (raw.ptr & 1) ? ptrdiff_t(raw.ptr - 1) : -1;
#endif
}

template <typename T>
bool PatchSlot(void** slot, T replacement, T& original) {
#if defined(_WIN32)
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
    return false;
  }
  original = reinterpret_cast<T>(*slot);
  *slot = reinterpret_cast<void*>(replacement);
  VirtualProtect(slot, sizeof(void*), old, &old);
#else
  const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
  void* start = reinterpret_cast<void*>(uintptr_t(slot) & ~(page - 1));
  if (mprotect(start, page * 2, PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  original = reinterpret_cast<T>(*slot);
  *slot = reinterpret_cast<void*>(replacement);
  mprotect(start, page * 2, PROT_READ);
#endif
  return true;
}

// Replaces the virtual function at byte offset `offset` of `object`'s vtable.
template <typename T>
bool PatchVirtual(void* object, ptrdiff_t offset, T replacement, T& original) {
  if (!object || offset < 0) {
    return false;
  }
  void** vtable = *reinterpret_cast<void***>(object);
  return PatchSlot(vtable + offset / ptrdiff_t(sizeof(void*)), replacement, original);
}

}  // namespace nhl3on3
