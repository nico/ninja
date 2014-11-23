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

#ifndef NINJA_PLAN_H_
#define NINJA_PLAN_H_

#include <map>
#include <set>
#include <string>
#include <vector>
using namespace std;

struct DependencyScan;
struct Edge;
struct Node;

/// Plan stores the state of a build plan: what we intend to build,
/// which steps we're ready to execute.
struct Plan {
  Plan();

  /// Add a target to our plan (including all its dependencies).
  /// Returns false if we don't need to build this target; may
  /// fill in |err| with an error message if there's a problem.
  bool AddTarget(Node* node, string* err);

  // Pop a ready edge off the queue of edges to build.
  // Returns NULL if there's no work to do.
  Edge* FindWork();

  /// Returns true if there's more work to be done.
  bool more_to_do() const { return wanted_edges_ > 0 && command_edges_ > 0; }

  /// Dumps the current state of the plan.
  void Dump();

  /// Mark an edge as done building.  Used internally and by
  /// tests.
  void EdgeFinished(Edge* edge);

  /// Clean the given node during the build.
  void CleanNode(DependencyScan* scan, Node* node);

  /// Number of edges with commands to run.
  int command_edge_count() const { return command_edges_; }

 private:
  bool AddSubTarget(Node* node, vector<Node*>* stack, string* err);
  bool CheckDependencyCycle(Node* node, vector<Node*>* stack, string* err);
  void NodeFinished(Node* node);

  /// Submits a ready edge as a candidate for execution.
  /// The edge may be delayed from running, for example if it's a member of a
  /// currently-full pool.
  void ScheduleWork(Edge* edge);

  /// Allows jobs blocking on |edge| to potentially resume.
  /// For example, if |edge| is a member of a pool, calling this may schedule
  /// previously pending jobs in that pool.
  void ResumeDelayedJobs(Edge* edge);

  /// Keep track of which edges we want to build in this plan.  If this map does
  /// not contain an entry for an edge, we do not want to build the entry or its
  /// dependents.  If an entry maps to false, we do not want to build it, but we
  /// might want to build one of its dependents.  If the entry maps to true, we
  /// want to build it.
  map<Edge*, bool> want_;

  set<Edge*> ready_;

  /// Total number of edges that have commands (not phony).
  int command_edges_;

  /// Total remaining number of wanted edges.
  int wanted_edges_;
};

#endif  // NINJA_PLAN_H_
