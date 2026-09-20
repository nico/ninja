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

#include <assert.h>

#include "eval_env.h"

using namespace std;

std::string BindingEnv::LookupVariable(StringPiece var) {
  // This is called by the thread that owns this scope, or by anyone once
  // parsing is done. Both see everything in this scope.
  bool found;
  return LookupVariableAt(var, INT_MAX, &found);
}

std::string BindingEnv::LookupVariableAt(StringPiece var, int snapshot,
                                         bool* found) {
  *found = true;
  const auto i = bindings_.find(var);
  if (i != bindings_.end())
    return i->second;
  if (concurrent_ && concurrent_->num_late_bindings.load() > 0) {
    // The owning thread is the only one that writes, so it could read without
    // the lock. It's not worth distinguishing the two cases.
    std::lock_guard<std::mutex> lock(concurrent_->mutex);
    const auto late = concurrent_->late_bindings.find(var);
    if (late != concurrent_->late_bindings.end()) {
      if (late->second.version <= snapshot)
        return late->second.value;
      // If the binding was added after the snapshot, it doesn't exist as far
      // as the caller is concerned. Else the caller should get an older value,
      // which is gone.
      if (late->second.first_version <= snapshot) {
        concurrent_->conflict->store(true);
        return late->second.value;
      }
    }
  }
  if (parent_)
    return parent_->LookupVariableAt(var, parent_snapshot_, found);
  *found = false;
  return "";
}

void BindingEnv::AddBinding(const std::string& key, StringPiece val) {
  if (concurrent_) {
    if (bindings_.find(key) != bindings_.end()) {
      // Children may be reading the old value without synchronization, and
      // children created earlier must keep seeing the old value.
      concurrent_->conflict->store(true);
      return;
    }
    // Overwriting a value that was added since BeginConcurrentAccess() is
    // fine: children that should still see the old value notice that when
    // they look it up.
    std::lock_guard<std::mutex> lock(concurrent_->mutex);
    auto& entry = concurrent_->late_bindings[key];
    entry.value.assign(val.begin(), val.end());
    entry.version = ++concurrent_->version;
    if (entry.first_version == 0)
      entry.first_version = entry.version;
    concurrent_->num_late_bindings.store(
        static_cast<int>(concurrent_->late_bindings.size()));
    return;
  }
  bindings_[key].assign(val.begin(), val.end());
}

void BindingEnv::AddRule(std::unique_ptr<const Rule> rule) {
  assert(LookupRuleCurrentScope(rule->name()) == NULL);
  if (concurrent_) {
    std::lock_guard<std::mutex> lock(concurrent_->mutex);
    auto& entry = concurrent_->late_rules[rule->name()];
    entry.first = std::move(rule);
    entry.second = ++concurrent_->version;
    concurrent_->num_late_rules.store(
        static_cast<int>(concurrent_->late_rules.size()));
    return;
  }
  rules_[rule->name()] = std::move(rule);
}

const Rule* BindingEnv::LookupRuleCurrentScope(StringPiece rule_name) {
  auto i = rules_.find(rule_name);
  if (i != rules_.end())
    return i->second.get();
  if (concurrent_) {
    // Only called on the owning thread, which sees all its additions.
    std::lock_guard<std::mutex> lock(concurrent_->mutex);
    auto late = concurrent_->late_rules.find(rule_name);
    if (late != concurrent_->late_rules.end())
      return late->second.first.get();
  }
  return NULL;
}

const Rule* BindingEnv::LookupRule(StringPiece rule_name) {
  return LookupRuleAt(rule_name, INT_MAX);
}

const Rule* BindingEnv::LookupRuleAt(StringPiece rule_name, int snapshot) {
  auto i = rules_.find(rule_name);
  if (i != rules_.end())
    return i->second.get();
  if (concurrent_ && concurrent_->num_late_rules.load() > 0) {
    std::lock_guard<std::mutex> lock(concurrent_->mutex);
    auto late = concurrent_->late_rules.find(rule_name);
    // Rules that were added after the snapshot don't exist as far as the
    // caller is concerned. (Rules never change.)
    if (late != concurrent_->late_rules.end() &&
        late->second.second <= snapshot) {
      return late->second.first.get();
    }
  }
  if (parent_)
    return parent_->LookupRuleAt(rule_name, parent_snapshot_);
  return NULL;
}

void BindingEnv::BeginConcurrentAccess(std::atomic<bool>* conflict) {
  assert(!concurrent_);
  concurrent_.reset(new Concurrent);
  concurrent_->conflict = conflict;
  concurrent_->num_late_bindings.store(0);
  concurrent_->num_late_rules.store(0);
  concurrent_->version = 0;
}

void BindingEnv::EndConcurrentAccess(bool keep_additions) {
  assert(concurrent_);
  if (keep_additions) {
    for (auto& binding : concurrent_->late_bindings)
      bindings_[binding.first] = std::move(binding.second.value);
    for (auto& rule : concurrent_->late_rules)
      rules_[rule.first] = std::move(rule.second.first);
  }
  concurrent_.reset();
  // Everything in parent_ is visible from now on.
  parent_snapshot_ = INT_MAX;
}

int BindingEnv::version() const {
  return concurrent_ ? concurrent_->version : 0;
}

void Rule::AddBinding(const string& key, const EvalString& val) {
  bindings_[key] = val;
}

const EvalString* Rule::GetBinding(StringPiece key) const {
  Bindings::const_iterator i = bindings_.find(key);
  if (i == bindings_.end())
    return NULL;
  return &i->second;
}

std::unique_ptr<Rule> Rule::Phony() {
  auto rule = std::unique_ptr<Rule>(new Rule("phony"));
  rule->phony_ = true;
  return rule;
}

bool Rule::IsPhony() const {
  return phony_;
}

// static
bool Rule::IsReservedBinding(StringPiece var) {
  return var == "command" ||
      var == "depfile" ||
      var == "dyndep" ||
      var == "description" ||
      var == "deps" ||
      var == "generator" ||
      var == "pool" ||
      var == "restat" ||
      var == "rspfile" ||
      var == "rspfile_content" ||
      var == "msvc_deps_prefix";
}

const map<string, std::unique_ptr<const Rule>, StringPieceLess>& BindingEnv::GetRules() const {
  return rules_;
}

std::string BindingEnv::LookupWithFallback(StringPiece var,
                                           const EvalString* eval,
                                           Env* env) {
  const auto i = bindings_.find(var);
  if (i != bindings_.end())
    return i->second;
  if (concurrent_ && concurrent_->num_late_bindings.load() > 0) {
    // Only edges use this, on the thread that owns this scope.
    std::lock_guard<std::mutex> lock(concurrent_->mutex);
    const auto late = concurrent_->late_bindings.find(var);
    if (late != concurrent_->late_bindings.end())
      return late->second.value;
  }

  if (eval)
    return eval->Evaluate(env);

  if (parent_) {
    bool found;
    return parent_->LookupVariableAt(var, parent_snapshot_, &found);
  }

  return "";
}

string EvalString::Evaluate(Env* env) const {
  if (parsed_.empty()) {
    return single_token_;
  }

  string result;
  for (TokenList::const_iterator i = parsed_.begin(); i != parsed_.end(); ++i) {
    if (i->second == RAW)
      result.append(i->first);
    else
      result.append(env->LookupVariable(i->first));
  }
  return result;
}

void EvalString::AddText(StringPiece text) {
  if (parsed_.empty()) {
    single_token_.append(text.begin(), text.end());
  } else if (!parsed_.empty() && parsed_.back().second == RAW) {
    parsed_.back().first.append(text.begin(), text.end());
  } else {
    parsed_.push_back(std::make_pair(text.AsString(), RAW));
  }
}

void EvalString::AddSpecial(StringPiece text) {
  if (parsed_.empty() && !single_token_.empty()) {
    // Going from one to two tokens, so we can no longer apply
    // our single_token_ optimization and need to push everything
    // onto the vector.
    parsed_.push_back(std::make_pair(std::move(single_token_), RAW));
  }
  parsed_.push_back(std::make_pair(text.AsString(), SPECIAL));
}

string EvalString::Serialize() const {
  string result;
  if (parsed_.empty() && !single_token_.empty()) {
    result.append("[");
    result.append(single_token_);
    result.append("]");
  } else {
    for (const auto& pair : parsed_) {
      result.append("[");
      if (pair.second == SPECIAL)
        result.append("$");
      result.append(pair.first.begin(), pair.first.end());
      result.append("]");
    }
  }
  return result;
}

string EvalString::Unparse() const {
  string result;
  if (parsed_.empty() && !single_token_.empty()) {
    result.append(single_token_.begin(), single_token_.end());
  } else {
    for (TokenList::const_iterator i = parsed_.begin();
         i != parsed_.end(); ++i) {
      bool special = (i->second == SPECIAL);
      if (special)
        result.append("${");
      result.append(i->first.begin(), i->first.end());
      if (special)
        result.append("}");
    }
  }
  return result;
}
