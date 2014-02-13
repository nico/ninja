//===--- StringMap.cpp - String Hash table map implementation -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the StringMap class.
//
//===----------------------------------------------------------------------===//

#include "StringMap.h"
#include <cassert>
using namespace llvm;

//#include "../hash_map.h"

#if (__GNUC__ >= 4)
#define LLVM_LIKELY(EXPR) __builtin_expect((bool)(EXPR), true)
#else
#define LLVM_LIKELY(EXPR) (EXPR)
#endif

// Hash function for strings.
//
// This is the Bernstein hash function.
//
// FIXME: Investigate whether a modified bernstein hash function performs
// better: http://eternallyconfuzzled.com/tuts/algorithms/jsw_tut_hashing.aspx
//   X*33+c -> X*33^c
static inline unsigned HashString(StringPiece Str, unsigned Result = 0) {
  for (unsigned i = 0, e = Str.len_; i != e; ++i)
    Result = Result * 33 + (unsigned char)Str.str_[i];
  return Result;
}

StringMapImpl::StringMapImpl(unsigned InitSize, unsigned itemSize) {
  ItemSize = itemSize;
  
  // If a size is specified, initialize the table with that many buckets.
  if (InitSize) {
    init(InitSize);
    return;
  }
  
  // Otherwise, initialize it with zero buckets to avoid the allocation.
  TheTable = 0;
  NumBuckets = 0;
  NumItems = 0;
  NumTombstones = 0;
}

void StringMapImpl::init(unsigned InitSize) {
  assert((InitSize & (InitSize-1)) == 0 &&
         "Init Size must be a power of 2 or zero!");
  NumBuckets = InitSize ? InitSize : 16;
  NumItems = 0;
  NumTombstones = 0;
  
  TheTable = (StringMapEntryBase **)calloc(NumBuckets+1,
                                           sizeof(StringMapEntryBase **) +
                                           sizeof(unsigned));

  // Allocate one extra bucket, set it to look filled so the iterators stop at
  // end.
  TheTable[NumBuckets] = (StringMapEntryBase*)2;
}


// Look up the bucket that the specified string should end up in.  If it
// already exists as a key in the map, the Item pointer for the specified
// bucket will be non-null.  Otherwise, it will be null.  In either case, the
// FullHashValue field of the bucket will be set to the hash value of the
// string.
unsigned StringMapImpl::LookupBucketFor(StringPiece Name) {
  unsigned HTSize = NumBuckets;
  if (HTSize == 0) {  // Hash table unallocated so far?
    init(16);
    HTSize = NumBuckets;
  }
  unsigned FullHashValue = HashString(Name);
  //unsigned FullHashValue = MurmurHash2(Name.str_, Name.len_);
  unsigned BucketNo = FullHashValue & (HTSize-1);
  unsigned *HashTable = (unsigned *)(TheTable + NumBuckets + 1);

  unsigned ProbeAmt = 1;
  int FirstTombstone = -1;
  while (1) {
    StringMapEntryBase *BucketItem = TheTable[BucketNo];
    // If we found an empty bucket, this key isn't in the table yet, return it.
    if (LLVM_LIKELY(BucketItem == 0)) {
      // If we found a tombstone, we want to reuse the tombstone instead of an
      // empty bucket.  This reduces probing.
      if (FirstTombstone != -1) {
        HashTable[FirstTombstone] = FullHashValue;
        return FirstTombstone;
      }
      
      HashTable[BucketNo] = FullHashValue;
      return BucketNo;
    }
    
    if (BucketItem == getTombstoneVal()) {
      // Skip over tombstones.  However, remember the first one we see.
      if (FirstTombstone == -1) FirstTombstone = BucketNo;
    } else if (LLVM_LIKELY(HashTable[BucketNo] == FullHashValue)) {
      // If the full hash value matches, check deeply for a match.  The common
      // case here is that we are only looking at the buckets (for item info
      // being non-null and for the full hash value) not at the items.  This
      // is important for cache locality.
      
      // Do the comparison like this because Name isn't necessarily
      // null-terminated!
      char *ItemStr = (char*)BucketItem+ItemSize;
      if (Name == StringPiece(ItemStr, BucketItem->getKeyLength())) {
        // We found a match!
        return BucketNo;
      }
    }
    
    // Okay, we didn't find the item.  Probe to the next bucket.
    BucketNo = (BucketNo+ProbeAmt) & (HTSize-1);
    
    // Use quadratic probing, it has fewer clumping artifacts than linear
    // probing and has good cache behavior in the common case.
    ++ProbeAmt;
  }
}



// Grow the table, redistributing values into the buckets with the appropriate
// mod-of-hashtable-size.
void StringMapImpl::RehashTable() {
  unsigned NewSize;
  unsigned *HashTable = (unsigned *)(TheTable + NumBuckets + 1);

  // If the hash table is now more than 3/4 full, or if fewer than 1/8 of
  // the buckets are empty (meaning that many are filled with tombstones),
  // grow/rehash the table.
  if (NumItems*4 > NumBuckets*3) {
    NewSize = NumBuckets*2;
  } else if (NumBuckets-(NumItems+NumTombstones) <= NumBuckets/8) {
    NewSize = NumBuckets;
  } else {
    return;
  }

  // Allocate one extra bucket which will always be non-empty.  This allows the
  // iterators to stop at end.
  StringMapEntryBase **NewTableArray =
    (StringMapEntryBase **)calloc(NewSize+1, sizeof(StringMapEntryBase *) +
                                             sizeof(unsigned));
  unsigned *NewHashArray = (unsigned *)(NewTableArray + NewSize + 1);
  NewTableArray[NewSize] = (StringMapEntryBase*)2;

  // Rehash all the items into their new buckets.  Luckily :) we already have
  // the hash values available, so we don't have to rehash any strings.
  for (unsigned I = 0, E = NumBuckets; I != E; ++I) {
    StringMapEntryBase *Bucket = TheTable[I];
    if (Bucket && Bucket != getTombstoneVal()) {
      // Fast case, bucket available.
      unsigned FullHash = HashTable[I];
      unsigned NewBucket = FullHash & (NewSize-1);
      if (NewTableArray[NewBucket] == 0) {
        NewTableArray[FullHash & (NewSize-1)] = Bucket;
        NewHashArray[FullHash & (NewSize-1)] = FullHash;
        continue;
      }
      
      // Otherwise probe for a spot.
      unsigned ProbeSize = 1;
      do {
        NewBucket = (NewBucket + ProbeSize++) & (NewSize-1);
      } while (NewTableArray[NewBucket]);
      
      // Finally found a slot.  Fill it in.
      NewTableArray[NewBucket] = Bucket;
      NewHashArray[NewBucket] = FullHash;
    }
  }
  
  free(TheTable);
  
  TheTable = NewTableArray;
  NumBuckets = NewSize;
  NumTombstones = 0;
}
