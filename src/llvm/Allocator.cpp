//===--- Allocator.cpp - Simple memory allocation abstraction -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the BumpPtrAllocator interface.
//
//===----------------------------------------------------------------------===//

#include "Allocator.h"
#include <cstring>

namespace llvm {

BumpPtrAllocator::BumpPtrAllocator(size_t size, size_t threshold)
    : SlabSize(size), SizeThreshold(std::min(size, threshold)),
      CurSlab(0), BytesAllocated(0) { }

BumpPtrAllocator::~BumpPtrAllocator() {
  DeallocateSlabs(CurSlab);
}

// Align Ptr to Alignment bytes, rounding up.  Alignment should be a power of
// two.  This method rounds up, so AlignPtr(7, 4) == 8 and AlignPtr(8, 4) == 8.
char *BumpPtrAllocator::AlignPtr(char *Ptr, size_t Alignment) {
  assert(Alignment && (Alignment & (Alignment - 1)) == 0 &&
         "Alignment is not a power of two!");
  return (char *)(((uintptr_t)Ptr + Alignment - 1) &
                  ~(uintptr_t)(Alignment - 1));
}

// Allocate a new slab and move the bump pointers over into the new slab.
// Modifies CurPtr and End.
void BumpPtrAllocator::StartNewSlab() {
  // If we allocated a big number of slabs already it's likely that we're going
  // to allocate more. Increase slab size to reduce mallocs and possibly memory
  // overhead. The factors are chosen conservatively to avoid overallocation.
  if (BytesAllocated >= SlabSize * 128)
    SlabSize *= 2;

  MemSlab *NewSlab = AllocateSlab(SlabSize);
  NewSlab->NextPtr = CurSlab;
  CurSlab = NewSlab;
  CurPtr = (char*)(CurSlab + 1);
  End = ((char*)CurSlab) + CurSlab->Size;
}

// Deallocate all memory slabs after and including this one.
void BumpPtrAllocator::DeallocateSlabs(MemSlab *Slab) {
  while (Slab) {
    MemSlab *NextSlab = Slab->NextPtr;
    DeallocateSlab(Slab);
    Slab = NextSlab;
  }
}

MemSlab *BumpPtrAllocator::AllocateSlab(size_t Size) {
  MemSlab *Slab = (MemSlab*)malloc(Size);
  Slab->Size = Size;
  Slab->NextPtr = 0;
  return Slab;
}

void BumpPtrAllocator::DeallocateSlab(MemSlab *Slab) {
  free(Slab);
}

// Deallocate all but the current slab and reset the current pointer to the
// beginning of it, freeing all memory allocated so far.
void BumpPtrAllocator::Reset() {
  if (!CurSlab)
    return;
  DeallocateSlabs(CurSlab->NextPtr);
  CurSlab->NextPtr = 0;
  CurPtr = (char*)(CurSlab + 1);
  End = ((char*)CurSlab) + CurSlab->Size;
  BytesAllocated = 0;
}

// Allocate space at the specified alignment.
void *BumpPtrAllocator::Allocate(size_t Size, size_t Alignment) {
  if (!CurSlab) // Start a new slab if we haven't allocated one already.
    StartNewSlab();

  // Keep track of how many bytes we've allocated.
  BytesAllocated += Size;

  // 0-byte alignment means 1-byte alignment.
  if (Alignment == 0) Alignment = 1;

  // Allocate the aligned space, going forwards from CurPtr.
  char *Ptr = AlignPtr(CurPtr, Alignment);

  // Check if we can hold it.
  if (Ptr + Size <= End) {
    CurPtr = Ptr + Size;
    return Ptr;
  }

  // If Size is really big, allocate a separate slab for it.
  size_t PaddedSize = Size + sizeof(MemSlab) + Alignment - 1;
  if (PaddedSize > SizeThreshold) {
    MemSlab *NewSlab = AllocateSlab(PaddedSize);

    // Put the new slab after the current slab, since we are not allocating
    // into it.
    NewSlab->NextPtr = CurSlab->NextPtr;
    CurSlab->NextPtr = NewSlab;

    Ptr = AlignPtr((char*)(NewSlab + 1), Alignment);
    assert((uintptr_t)Ptr + Size <= (uintptr_t)NewSlab + NewSlab->Size);
    return Ptr;
  }

  // Otherwise, start a new slab and try again.
  StartNewSlab();
  Ptr = AlignPtr(CurPtr, Alignment);
  CurPtr = Ptr + Size;
  assert(CurPtr <= End && "Unable to allocate memory!");
  return Ptr;
}

unsigned BumpPtrAllocator::GetNumSlabs() const {
  unsigned NumSlabs = 0;
  for (MemSlab *Slab = CurSlab; Slab != 0; Slab = Slab->NextPtr)
    ++NumSlabs;
  return NumSlabs;
}

size_t BumpPtrAllocator::getTotalMemory() const {
  size_t TotalMemory = 0;
  for (MemSlab *Slab = CurSlab; Slab != 0; Slab = Slab->NextPtr)
    TotalMemory += Slab->Size;
  return TotalMemory;
}
  
}
