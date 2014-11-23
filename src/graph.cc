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

#include "graph.h"

#include <assert.h>
#include <stdio.h>

#include "build_log.h"
#include "disk_interface.h"
#include "metrics.h"
#include "state.h"
#include "util.h"

bool Node::Stat(DiskInterface* disk_interface) {
  METRIC_RECORD("node stat");
  mtime_ = disk_interface->Stat(path_);
  return mtime_ > 0;
}

void Rule::AddBinding(const string& key, const EvalString& val) {
  bindings_[key] = val;
}

const EvalString* Rule::GetBinding(const string& key) const {
  map<string, EvalString>::const_iterator i = bindings_.find(key);
  if (i == bindings_.end())
    return NULL;
  return &i->second;
}

// static
bool Rule::IsReservedBinding(const string& var) {
  return var == "command" ||
      var == "depfile" ||
      var == "description" ||
      var == "deps" ||
      var == "generator" ||
      var == "pool" ||
      var == "restat" ||
      var == "rspfile" ||
      var == "rspfile_content";
}

bool Edge::AllInputsReady() const {
  for (vector<Node*>::const_iterator i = inputs_.begin();
       i != inputs_.end(); ++i) {
    if ((*i)->in_edge() && !(*i)->in_edge()->outputs_ready())
      return false;
  }
  return true;
}

/// An Env for an Edge, providing $in and $out.
struct EdgeEnv : public Env {
  enum EscapeKind { kShellEscape, kDoNotEscape };

  explicit EdgeEnv(Edge* edge, EscapeKind escape)
      : edge_(edge), escape_in_out_(escape) {}
  virtual string LookupVariable(const string& var);

  /// Given a span of Nodes, construct a list of paths suitable for a command
  /// line.
  string MakePathList(vector<Node*>::iterator begin,
                      vector<Node*>::iterator end,
                      char sep);

  Edge* edge_;
  EscapeKind escape_in_out_;
};

string EdgeEnv::LookupVariable(const string& var) {
  if (var == "in" || var == "in_newline") {
    int explicit_deps_count = edge_->inputs_.size() - edge_->implicit_deps_ -
      edge_->order_only_deps_;
    return MakePathList(edge_->inputs_.begin(),
                        edge_->inputs_.begin() + explicit_deps_count,
                        var == "in" ? ' ' : '\n');
  } else if (var == "out") {
    return MakePathList(edge_->outputs_.begin(),
                        edge_->outputs_.end(),
                        ' ');
  }

  // See notes on BindingEnv::LookupWithFallback.
  const EvalString* eval = edge_->rule_->GetBinding(var);
  return edge_->env_->LookupWithFallback(var, eval, this);
}

string EdgeEnv::MakePathList(vector<Node*>::iterator begin,
                             vector<Node*>::iterator end,
                             char sep) {
  string result;
  for (vector<Node*>::iterator i = begin; i != end; ++i) {
    if (!result.empty())
      result.push_back(sep);
    const string& path = (*i)->PathDecanonicalized();
    if (escape_in_out_ == kShellEscape) {
#if _WIN32
      GetWin32EscapedString(path, &result);
#else
      GetShellEscapedString(path, &result);
#endif
    } else {
      result.append(path);
    }
  }
  return result;
}

string Edge::EvaluateCommand(bool incl_rsp_file) {
  string command = GetBinding("command");
  if (incl_rsp_file) {
    string rspfile_content = GetBinding("rspfile_content");
    if (!rspfile_content.empty())
      command += ";rspfile=" + rspfile_content;
  }
  return command;
}

string Edge::GetBinding(const string& key) {
  EdgeEnv env(this, EdgeEnv::kShellEscape);
  return env.LookupVariable(key);
}

bool Edge::GetBindingBool(const string& key) {
  return !GetBinding(key).empty();
}

string Edge::GetUnescapedDepfile() {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("depfile");
}

string Edge::GetUnescapedRspfile() {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("rspfile");
}

void Edge::Dump(const char* prefix) const {
  printf("%s[ ", prefix);
  for (vector<Node*>::const_iterator i = inputs_.begin();
       i != inputs_.end() && *i != NULL; ++i) {
    printf("%s ", (*i)->path().c_str());
  }
  printf("--%s-> ", rule_->name().c_str());
  for (vector<Node*>::const_iterator i = outputs_.begin();
       i != outputs_.end() && *i != NULL; ++i) {
    printf("%s ", (*i)->path().c_str());
  }
  if (pool_) {
    if (!pool_->name().empty()) {
      printf("(in pool '%s')", pool_->name().c_str());
    }
  } else {
    printf("(null pool?)");
  }
  printf("] 0x%p\n", this);
}

bool Edge::is_phony() const {
  return rule_ == &State::kPhonyRule;
}

bool Edge::use_console() const {
  return pool() == &State::kConsolePool;
}

string Node::PathDecanonicalized() const {
  string result = path_;
#ifdef _WIN32
  unsigned int mask = 1;
  for (char* c = &result[0]; (c = strchr(c, '/')) != NULL;) {
    if (slash_bits_ & mask)
      *c = '\\';
    c++;
    mask <<= 1;
  }
#endif
  return result;
}

void Node::Dump(const char* prefix) const {
  printf("%s <%s 0x%p> mtime: %d%s, (:%s), ",
         prefix, path().c_str(), this,
         mtime(), mtime() ? "" : " (:missing)",
         dirty() ? " dirty" : " clean");
  if (in_edge()) {
    in_edge()->Dump("in-edge: ");
  } else {
    printf("no in-edge\n");
  }
  printf(" out edges:\n");
  for (vector<Edge*>::const_iterator e = out_edges().begin();
       e != out_edges().end() && *e != NULL; ++e) {
    (*e)->Dump(" +- ");
  }
}
