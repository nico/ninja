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

#ifndef NINJA_GRAPH_H_
#define NINJA_GRAPH_H_

#include <string>
#include <vector>
using namespace std;

#include "eval_env.h"
#include "timestamp.h"

struct DiskInterface;
struct Edge;
struct Node;
struct Pool;

/// Information about a node in the dependency graph: the file, whether
/// it's dirty, mtime, etc.
struct Node {
  Node(const string& path, unsigned int slash_bits)
      : path_(path),
        slash_bits_(slash_bits),
        mtime_(-1),
        dirty_(false),
        in_edge_(NULL),
        id_(-1) {}

  /// Return true if the file exists (mtime_ got a value).
  bool Stat(DiskInterface* disk_interface);

  /// Return true if we needed to stat.
  bool StatIfNecessary(DiskInterface* disk_interface) {
    if (status_known())
      return false;
    Stat(disk_interface);
    return true;
  }

  /// Mark as not-yet-stat()ed and not dirty.
  void ResetState() {
    mtime_ = -1;
    dirty_ = false;
  }

  /// Mark the Node as already-stat()ed and missing.
  void MarkMissing() {
    mtime_ = 0;
  }

  bool exists() const {
    return mtime_ != 0;
  }

  bool status_known() const {
    return mtime_ != -1;
  }

  const string& path() const { return path_; }
  /// Get |path()| but use slash_bits to convert back to original slash styles.
  string PathDecanonicalized() const;
  unsigned int slash_bits() const { return slash_bits_; }
  TimeStamp mtime() const { return mtime_; }

  bool dirty() const { return dirty_; }
  void set_dirty(bool dirty) { dirty_ = dirty; }
  void MarkDirty() { dirty_ = true; }

  Edge* in_edge() const { return in_edge_; }
  void set_in_edge(Edge* edge) { in_edge_ = edge; }

  int id() const { return id_; }
  void set_id(int id) { id_ = id; }

  const vector<Edge*>& out_edges() const { return out_edges_; }
  void AddOutEdge(Edge* edge) { out_edges_.push_back(edge); }

  void Dump(const char* prefix="") const;

private:
  string path_;

  /// Set bits starting from lowest for backslashes that were normalized to
  /// forward slashes by CanonicalizePath. See |PathDecanonicalized|.
  unsigned int slash_bits_;

  /// Possible values of mtime_:
  ///   -1: file hasn't been examined
  ///   0:  we looked, and file doesn't exist
  ///   >0: actual file's mtime
  TimeStamp mtime_;

  /// Dirty is true when the underlying file is out-of-date.
  /// But note that Edge::outputs_ready_ is also used in judging which
  /// edges to build.
  bool dirty_;

  /// The Edge that produces this Node, or NULL when there is no
  /// known edge to produce it.
  Edge* in_edge_;

  /// All Edges that use this Node as an input.
  vector<Edge*> out_edges_;

  /// A dense integer id for the node, assigned and used by DepsLog.
  int id_;
};

/// An invokable build command and associated metadata (description, etc.).
struct Rule {
  explicit Rule(const string& name) : name_(name) {}

  const string& name() const { return name_; }

  typedef map<string, EvalString> Bindings;
  void AddBinding(const string& key, const EvalString& val);

  static bool IsReservedBinding(const string& var);

  const EvalString* GetBinding(const string& key) const;

 private:
  // Allow the parsers to reach into this object and fill out its fields.
  friend struct ManifestParser;

  string name_;
  map<string, EvalString> bindings_;
};

/// An edge in the dependency graph; links between Nodes using Rules.
struct Edge {
  Edge() : rule_(NULL), env_(NULL), outputs_ready_(false), deps_missing_(false),
           implicit_deps_(0), order_only_deps_(0) {}

  /// Return true if all inputs' in-edges are ready.
  bool AllInputsReady() const;

  /// Expand all variables in a command and return it as a string.
  /// If incl_rsp_file is enabled, the string will also contain the
  /// full contents of a response file (if applicable)
  string EvaluateCommand(bool incl_rsp_file = false);

  /// Returns the shell-escaped value of |key|.
  string GetBinding(const string& key);
  bool GetBindingBool(const string& key);

  /// Like GetBinding("depfile"), but without shell escaping.
  string GetUnescapedDepfile();
  /// Like GetBinding("rspfile"), but without shell escaping.
  string GetUnescapedRspfile();

  void Dump(const char* prefix="") const;

  const Rule* rule_;
  Pool* pool_;
  vector<Node*> inputs_;
  vector<Node*> outputs_;
  BindingEnv* env_;
  bool outputs_ready_;
  bool deps_missing_;

  const Rule& rule() const { return *rule_; }
  Pool* pool() const { return pool_; }
  int weight() const { return 1; }
  bool outputs_ready() const { return outputs_ready_; }

  // There are three types of inputs.
  // 1) explicit deps, which show up as $in on the command line;
  // 2) implicit deps, which the target depends on implicitly (e.g. C headers),
  //                   and changes in them cause the target to rebuild;
  // 3) order-only deps, which are needed before the target builds but which
  //                     don't cause the target to rebuild.
  // These are stored in inputs_ in that order, and we keep counts of
  // #2 and #3 when we need to access the various subsets.
  int implicit_deps_;
  int order_only_deps_;
  bool is_implicit(size_t index) {
    return index >= inputs_.size() - order_only_deps_ - implicit_deps_ &&
        !is_order_only(index);
  }
  bool is_order_only(size_t index) {
    return index >= inputs_.size() - order_only_deps_;
  }

  bool is_phony() const;
  bool use_console() const;
};

#endif  // NINJA_GRAPH_H_
