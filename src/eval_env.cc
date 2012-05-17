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

#include "eval_env.h"

string EvalRope::AsString() const {
  string result;
  for (vector<StringPiece>::const_iterator i = pieces_.begin();
      i != pieces_.end(); ++i) {
    result.append(i->str_, i->len_);
  }
  return result;
}

bool EvalRope::operator==(const string& s) const {
  const char* p = s.data();
  int l = s.size();
  for (vector<StringPiece>::const_iterator i = pieces_.begin();
      i != pieces_.end(); ++i) {
    if (i->len_ > l)
      return false;
    if (memcmp(p, i->str_, i->len_) != 0)
      return false;
    l -= i->len_;
    p += i->len_;
  }
  return l == 0;
}

const EvalRope& BindingEnv::LookupVariable(StringPiece var) {
//string BindingEnv::LookupVariable(StringPiece var) {
  static EvalRope kEmptyRope;

  Bindings::iterator i = bindings_.find(var);
  if (i != bindings_.end())
    return i->second;
  if (parent_)
    return parent_->LookupVariable(var);
  return kEmptyRope;
}

void BindingEnv::AddBinding(StringPiece key, const EvalRope& val) {
//void BindingEnv::AddBinding(StringPiece key, const string& val) {
  bindings_[key] = val;
}

//string EvalString::Evaluate(Env* env) const {
//  string result;
//  for (TokenList::const_iterator i = parsed_.begin(); i != parsed_.end(); ++i) {
//    if (i->second == RAW)
//      result.append(i->first.str_, i->first.len_);
//    else
//      result.append(env->LookupVariable(i->first));
//  }
//  return result;
//}

EvalRope EvalString::Evaluate(Env* env) const {
  EvalRope result;
  for (TokenList::const_iterator i = parsed_.begin(); i != parsed_.end(); ++i) {
    if (i->second == RAW)
      result.AddPiece(i->first);
    else {
      const EvalRope& r = env->LookupVariable(i->first);
      result.AddPieces(r);
    }
  }
  return result;
}

void EvalString::AddText(StringPiece text, StringPool* pool) {
  // XXX add to last element in pool if prev was RAW
  //if (!parsed_.empty() && parsed_.back().second == RAW) {
    //if (pool)
      //text = pool->AddToLast(text, parsed.back().first);
    //parsed_.push_back(make_pair(text, RAW));
  //} else {
    if (pool)
      text = pool->Add(text);
    parsed_.push_back(make_pair(text, RAW));
  //}
}

void EvalString::AddSpecial(StringPiece text, StringPool* pool) {
  if (pool)
    text = pool->Add(text);
  parsed_.push_back(make_pair(text, SPECIAL));
}

string EvalString::Serialize() const {
  string result;
  for (TokenList::const_iterator i = parsed_.begin();
       i != parsed_.end(); ++i) {
    result.append("[");
    if (i->second == SPECIAL)
      result.append("$");
    result.append(i->first.str_, i->first.len_);
    result.append("]");
  }
  return result;
}
