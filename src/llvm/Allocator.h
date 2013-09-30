//===--- Allocator.h - Simple memory allocation abstraction -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the MallocAllocator and BumpPtrAllocator interfaces.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_ALLOCATOR_H
#define LLVM_SUPPORT_ALLOCATOR_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>

#include "../util.h"  // uint64_t

namespace llvm {

template <typename T>
struct AlignmentCalcImpl {
  char x;
  T t;
private:
  AlignmentCalcImpl() {} // Never instantiate.
};

/// AlignOf - A templated class that contains an enum value representing
///  the alignment of the template argument.  For example,
///  AlignOf<int>::Alignment represents the alignment of type "int".  The
///  alignment calculated is the minimum alignment, and not necessarily
///  the "desired" alignment returned by GCC's __alignof__ (for example).  Note
///  that because the alignment is an enum value, it can be used as a
///  compile-time constant (e.g., for template instantiation).
template <typename T>
struct AlignOf {
  enum { Alignment =
         static_cast<unsigned int>(sizeof(AlignmentCalcImpl<T>) - sizeof(T)) };
};

class MallocAllocator {
public:
  MallocAllocator() {}
  ~MallocAllocator() {}

  void Reset() {}

  void *Allocate(size_t Size, size_t /*Alignment*/) { return malloc(Size); }

  void Deallocate(const void *Ptr) { free(const_cast<void*>(Ptr)); }
};

/// MemSlab - This structure lives at the beginning of every slab allocated by
/// the bump allocator.
class MemSlab {
public:
  size_t Size;
  MemSlab *NextPtr;
};

/// BumpPtrAllocator - This allocator is useful for containers that need
/// very simple memory allocation strategies.  In particular, this just keeps
/// allocating memory, and never deletes it until the entire block is dead. This
/// makes allocation speedy, but must only be used when the trade-off is ok.
class BumpPtrAllocator {
  BumpPtrAllocator(const BumpPtrAllocator &);
  void operator=(const BumpPtrAllocator &);

  /// SlabSize - Allocate data into slabs of this size unless we get an
  /// allocation above SizeThreshold.
  size_t SlabSize;

  /// SizeThreshold - For any allocation larger than this threshold, we should
  /// allocate a separate slab.
  size_t SizeThreshold;

  MemSlab *AllocateSlab(size_t Size);
  void DeallocateSlab(MemSlab *Slab);

  /// CurSlab - The slab that we are currently allocating into.
  MemSlab *CurSlab;

  /// CurPtr - The current pointer into the current slab.  This points to the
  /// next free byte in the slab.
  char *CurPtr;

  /// End - The end of the current slab.
  ///
  char *End;

  /// BytesAllocated - This field tracks how many bytes we've allocated, so
  /// that we can compute how much space was wasted.
  size_t BytesAllocated;

  /// AlignPtr - Align Ptr to Alignment bytes, rounding up.  Alignment should
  /// be a power of two.  This method rounds up, so AlignPtr(7, 4) == 8 and
  /// AlignPtr(8, 4) == 8.
  static char *AlignPtr(char *Ptr, size_t Alignment);

  /// StartNewSlab - Allocate a new slab and move the bump pointers over into
  /// the new slab.  Modifies CurPtr and End.
  void StartNewSlab();

  /// DeallocateSlabs - Deallocate all memory slabs after and including this
  /// one.
  void DeallocateSlabs(MemSlab *Slab);

  template<typename T> friend class SpecificBumpPtrAllocator;
public:
  BumpPtrAllocator(size_t size = 4096, size_t threshold = 4096);
  ~BumpPtrAllocator();

  /// Reset - Deallocate all but the current slab and reset the current pointer
  /// to the beginning of it, freeing all memory allocated so far.
  void Reset();

  /// Allocate - Allocate space at the specified alignment.
  void *Allocate(size_t Size, size_t Alignment);

  void Deallocate(const void * /*Ptr*/) {}

  unsigned GetNumSlabs() const;

  /// Compute the total physical memory allocated by this allocator.
  size_t getTotalMemory() const;
};

}  // end namespace llvm

#endif // LLVM_SUPPORT_ALLOCATOR_H
