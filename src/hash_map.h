// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_MAP_H_
#define NINJA_MAP_H_

#include "string_piece.h"

// MurmurHash2, by Austin Appleby
static inline
unsigned int MurmurHash2(const void* key, size_t len) {
  static const unsigned int seed = 0xDECAFBAD;
  const unsigned int m = 0x5bd1e995;
  const int r = 24;
  unsigned int h = seed ^ len;
  const unsigned char * data = (const unsigned char *)key;
  while (len >= 4) {
    unsigned int k = *(unsigned int *)data;
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    len -= 4;
  }
  switch (len) {
  case 3: h ^= data[2] << 16;
  case 2: h ^= data[1] << 8;
  case 1: h ^= data[0];
    h *= m;
  };
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}


template <class V>
struct node {
  std::pair<StringPiece, V> val;
  node *next;
};

#define NHASH 98317

static inline unsigned hash(const char *p, unsigned l) {
#if 0
#define MULT 31
  unsigned h = 0;
  for (unsigned i = 0; i < l; ++i)
    h = MULT * h + *(p++);
  return h % NHASH;
#else
  return MurmurHash2(p, l) % NHASH;
#endif
}


/// A template for hash_maps keyed by a StringPiece whose string is
/// owned externally (typically by the values).  Use like:
/// ExternalStringHash<Foo*>::Type foos; to make foos into a hash
/// mapping StringPiece => Foo*.
template<typename V>
struct ExternalStringHashMap {
  class Type {
    node<V>** bin;
    int n_size;
   public:
    class Iterator {
      Type *container;
      int i;
      node<V>* n;
     public:
      Iterator(Type *container, int i, node<V>* p)
          : container(container), i(i), n(p) {
      }
      pair<StringPiece, V> *operator->() {
        return &n->val;
      }
      Iterator& operator++() {
        if (n->next) {
          n = n->next;
          return *this;
        }
        int j;
        for (j = i + 1; j < NHASH && container->bin[j] == NULL; ++j)
          ;
        if (j < NHASH) {
          i = j;
          n = container->bin[j];
          return *this;
        }
        i = NHASH;
        n = NULL;
        return *this;
      }
      bool operator!=(const Iterator &rhs) {
        return !(i == rhs.i && n == rhs.n);
      }
    };
    typedef Iterator iterator;
    typedef std::pair<StringPiece, V> value_type;

    Type() : n_size(0) {
      bin = (node<V>**)malloc(NHASH * sizeof(node<V>*));
      memset(bin, 0, NHASH * sizeof(node<V>*));
    }
    ~Type() {
      free(bin);
    }

    iterator find(StringPiece key) {
      unsigned h = hash(key.str_, key.len_);
      for (node<V>* p = bin[h]; p != NULL; p = p->next)
        if (p->val.first == key)
          return Iterator(this, h, p);
      return end();
    }

    V find_or_null(StringPiece key) {
      unsigned h = hash(key.str_, key.len_);
      for (node<V>* p = bin[h]; p != NULL; p = p->next)
        if (p->val.first == key)
          return p->val.second;
      return NULL;
    }

    iterator insert(value_type v) {
      unsigned h = hash(v.first.str_, v.first.len_);
      node<V>* p;
      for (p = bin[h]; p != NULL; p = p->next)
        if (p->val.first == v.first) {
          p->val.second = v.second;
          return Iterator(this, h, p);
        }
      p = (node<V> *) malloc(sizeof(node<V>));
      p->val = v;
      p->next = bin[h];
      bin[h] = p;
      ++n_size;
      return Iterator(this, h, p);
    }

    iterator begin() {
      int i;
      for (i = 0; i < NHASH && bin[i] == NULL; ++i)
        ;
      if (i < NHASH) return Iterator(this, i, bin[i]);
      return end();
    }

    iterator end() {
      return Iterator(this, NHASH, NULL);
    }
    //V& operator[](StringPiece key);

    size_t size() const {
      return n_size;
    }
    size_t bucket_count() {
      return NHASH;
    }
  };
};

#endif // NINJA_MAP_H_
