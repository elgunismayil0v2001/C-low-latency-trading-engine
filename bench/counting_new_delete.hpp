#pragma once

// Replaces the complete global operator new/delete family with malloc/free-backed versions that
// call TICKBOOK_ALLOC_HOOK() on every successful allocation. Include this header exactly once per
// executable, after the hook function is declared, and define TICKBOOK_ALLOC_HOOK first.
//
// Why the whole family and why noinline: replacing only operator new leaves the runtime's own
// operator delete releasing malloc memory, which ASan reports as alloc-dealloc-mismatch and GCC
// reports as -Wmismatched-new-delete. The out-of-line helpers also stop GCC from inlining a bare
// free() into operator delete and warning against the allocation side.

#ifndef TICKBOOK_ALLOC_HOOK
#error "define TICKBOOK_ALLOC_HOOK before including counting_new_delete.hpp"
#endif

#include <cstddef>
#include <cstdlib>
#include <new>

namespace tickbook_counting_alloc {

[[gnu::noinline]] inline void* allocate_plain(std::size_t size) noexcept {
  void* memory = std::malloc(size == 0 ? 1 : size);
  if (memory != nullptr) TICKBOOK_ALLOC_HOOK();
  return memory;
}

[[gnu::noinline]] inline void* allocate_aligned(std::size_t size,
                                                std::align_val_t alignment) noexcept {
  void* memory = nullptr;
  auto alignment_size = static_cast<std::size_t>(alignment);
  if (alignment_size < sizeof(void*)) alignment_size = sizeof(void*);
  if (posix_memalign(&memory, alignment_size, size == 0 ? alignment_size : size) != 0) {
    return nullptr;
  }
  TICKBOOK_ALLOC_HOOK();
  return memory;
}

[[gnu::noinline]] inline void release(void* memory) noexcept { std::free(memory); }

} // namespace tickbook_counting_alloc

void* operator new(std::size_t size) {
  if (void* memory = tickbook_counting_alloc::allocate_plain(size)) return memory;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
  if (void* memory = tickbook_counting_alloc::allocate_plain(size)) return memory;
  throw std::bad_alloc{};
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return tickbook_counting_alloc::allocate_plain(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return tickbook_counting_alloc::allocate_plain(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  if (void* memory = tickbook_counting_alloc::allocate_aligned(size, alignment)) return memory;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  if (void* memory = tickbook_counting_alloc::allocate_aligned(size, alignment)) return memory;
  throw std::bad_alloc{};
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return tickbook_counting_alloc::allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return tickbook_counting_alloc::allocate_aligned(size, alignment);
}

void operator delete(void* memory) noexcept { tickbook_counting_alloc::release(memory); }
void operator delete[](void* memory) noexcept { tickbook_counting_alloc::release(memory); }
void operator delete(void* memory, std::size_t) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete(void* memory, const std::nothrow_t&) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete(void* memory, std::align_val_t) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  tickbook_counting_alloc::release(memory);
}
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  tickbook_counting_alloc::release(memory);
}
