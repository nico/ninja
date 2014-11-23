// Copyright 2014 Google Inc. All Rights Reserved.
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

#ifndef NINJA_SCAN_H_
#define NINJA_SCAN_H_

#include <string>
#include <vector>
using namespace std;

struct BuildLog;
struct DiskInterface;
struct DepsLog;
struct Edge;
struct Node;
struct State;

/// ImplicitDepLoader loads implicit dependencies, as referenced via the
/// "depfile" attribute in build files.
struct ImplicitDepLoader {
  ImplicitDepLoader(State* state, DepsLog* deps_log,
                    DiskInterface* disk_interface)
      : state_(state), disk_interface_(disk_interface), deps_log_(deps_log) {}

  /// Load implicit dependencies for \a edge.
  /// @return false on error (without filling \a err if info is just missing
  //                          or out of date).
  bool LoadDeps(Edge* edge, string* err);

  DepsLog* deps_log() const {
    return deps_log_;
  }

 private:
  /// Load implicit dependencies for \a edge from a depfile attribute.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepFile(Edge* edge, const string& path, string* err);

  /// Load implicit dependencies for \a edge from the DepsLog.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepsFromLog(Edge* edge, string* err);

  /// Preallocate \a count spaces in the input array on \a edge, returning
  /// an iterator pointing at the first new space.
  vector<Node*>::iterator PreallocateSpace(Edge* edge, int count);

  /// If we don't have a edge that generates this input already,
  /// create one; this makes us not abort if the input is missing,
  /// but instead will rebuild in that circumstance.
  void CreatePhonyInEdge(Node* node);

  State* state_;
  DiskInterface* disk_interface_;
  DepsLog* deps_log_;
};


/// DependencyScan manages the process of scanning the files in a graph
/// and updating the dirty/outputs_ready state of all the nodes and edges.
struct DependencyScan {
  DependencyScan(State* state, BuildLog* build_log, DepsLog* deps_log,
                 DiskInterface* disk_interface)
      : build_log_(build_log),
        disk_interface_(disk_interface),
        dep_loader_(state, deps_log, disk_interface) {}

  /// Examine inputs, outputs, and command lines to judge whether an edge
  /// needs to be re-run, and update outputs_ready_ and each outputs' |dirty_|
  /// state accordingly.
  /// Returns false on failure.
  bool RecomputeDirty(Edge* edge, string* err);

  /// Recompute whether any output of the edge is dirty.
  /// Returns true if so.
  bool RecomputeOutputsDirty(Edge* edge, Node* most_recent_input);

  BuildLog* build_log() const {
    return build_log_;
  }
  void set_build_log(BuildLog* log) {
    build_log_ = log;
  }

  DepsLog* deps_log() const {
    return dep_loader_.deps_log();
  }

 private:
  /// Recompute whether a given single output should be marked dirty.
  /// Returns true if so.
  bool RecomputeOutputDirty(Edge* edge, Node* most_recent_input,
                            const string& command, Node* output);

  BuildLog* build_log_;
  DiskInterface* disk_interface_;
  ImplicitDepLoader dep_loader_;
};

#endif  // NINJA_SCAN_H_
