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

#ifndef NINJA_EVAL_ENV_H_
#define NINJA_EVAL_ENV_H_

#include <map>
#include <string>
#include <vector>
using namespace std;

#include "string_piece.h"
#include "string_pool.h"

/// An interface for a scope for variable (e.g. "$foo") lookups.
struct Env {
  virtual ~Env() {}
  virtual string LookupVariable(StringPiece var) = 0;
};

/// An Env which contains a mapping of variables to values
/// as well as a pointer to a parent scope.
struct BindingEnv : public Env {
  BindingEnv() : parent_(NULL) {}
  explicit BindingEnv(Env* parent) : parent_(parent) {}
  virtual ~BindingEnv() {}
  virtual string LookupVariable(StringPiece var);
  void AddBinding(const string& key, const string& val);

private:
  map<string, string> bindings_;
  Env* parent_;
};

/// A tokenized string that contains variable references.
/// Can be evaluated relative to an Env.
struct EvalString {
  string Evaluate(Env* env) const;

  void Clear() { parsed_.clear(); }
  bool empty() const { return parsed_.empty(); }

  void AddText(StringPiece text, StringPool* pool);
  void AddSpecial(StringPiece text, StringPool* pool);

  /// Construct a human-readable representation of the parsed state,
  /// for use in tests.
  string Serialize() const;

private:
  enum TokenType { RAW, SPECIAL };
  typedef vector<pair<StringPiece, TokenType> > TokenList;
  TokenList parsed_;
};

#endif  // NINJA_EVAL_ENV_H_
