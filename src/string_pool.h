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

#ifndef NINJA_STRING_POOL_H_
#define NINJA_STRING_POOL_H_

#include <string>
#include <vector>
using namespace std;

#if 1
class StringPool {
 public:
  StringPiece Add(StringPiece p) {
    pool_.push_back(p.AsString());
    return pool_.back();
  }

  StringPiece AddStr(const string& s) {
    pool_.push_back(s);
    return pool_.back();
  }
 private:
  // XXX bumpptr allocator instead
  vector<string> pool_;
};
#else
class StringPool {
 public:
  StringPool() {
    first_ = cur_ = new Slab;
  }

  // XXX free

  StringPiece Add(StringPiece p) {
    if (p.len_ > (int)sizeof(cur_->buf) - cur_->occ) {
fprintf(stderr, "slab realloc\n");
      cur_->next = new Slab;
      cur_ = cur_->next;
    }

    //pool_.push_back(p.AsString());
    //return pool_.back();
    char* dst = cur_->buf + cur_->occ;
    memcpy(dst, p.str_, p.len_);
    cur_->occ += p.len_;
    return StringPiece(dst, p.len_);
  }
 private:
  // XXX bumpptr allocator instead
  //vector<string> pool_;
  struct Slab {
    Slab() : next(NULL), occ(0) {}

    Slab* next;
    char buf[1 << 20];  // 1 MB
    int occ;
  };
  Slab* first_;
  Slab* cur_;
};
#endif

#endif  // NINJA_STRING_POOL_H_
