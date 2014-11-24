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

#include "scan.h"

#include <assert.h>
#include <stdio.h>

#include "build_log.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "debug_flags.h"
#include "disk_interface.h"
#include "graph.h"
#include "metrics.h"
#include "state.h"
#include "util.h"

bool ImplicitDepLoader::LoadDeps(Edge* edge, string* err) {
  string deps_type = edge->GetBinding("deps");
  if (!deps_type.empty())
    return LoadDepsFromLog(edge, err);

  string depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty())
    return LoadDepFile(edge, depfile, err);

  // No deps to load.
  return true;
}

bool ImplicitDepLoader::LoadDepFile(Edge* edge, const string& path,
                                    string* err) {
  METRIC_RECORD("depfile load");
  string content = disk_interface_->ReadFile(path, err);
  if (!err->empty()) {
    *err = "loading '" + path + "': " + *err;
    return false;
  }
  // On a missing depfile: return false and empty *err.
  if (content.empty()) {
    EXPLAIN("depfile '%s' is missing", path.c_str());
    return false;
  }

  DepfileParser depfile;
  string depfile_err;
  if (!depfile.Parse(&content, &depfile_err)) {
    *err = path + ": " + depfile_err;
    return false;
  }

  unsigned int unused;
  if (!CanonicalizePath(const_cast<char*>(depfile.out_.str_),
                        &depfile.out_.len_, &unused, err))
    return false;

  // Check that this depfile matches the edge's output.
  Node* first_output = edge->outputs_[0];
  StringPiece opath = StringPiece(first_output->path());
  if (opath != depfile.out_) {
    *err = "expected depfile '" + path + "' to mention '" +
        first_output->path() + "', got '" + depfile.out_.AsString() + "'";
    return false;
  }

  // Preallocate space in edge->inputs_ to be filled in below.
  vector<Node*>::iterator implicit_dep =
      PreallocateSpace(edge, depfile.ins_.size());

  // Add all its in-edges.
  for (vector<StringPiece>::iterator i = depfile.ins_.begin();
       i != depfile.ins_.end(); ++i, ++implicit_dep) {
    unsigned int slash_bits;
    if (!CanonicalizePath(const_cast<char*>(i->str_), &i->len_, &slash_bits,
                          err))
      return false;

    Node* node = state_->GetNode(*i, slash_bits);
    *implicit_dep = node;
    node->AddOutEdge(edge);
    CreatePhonyInEdge(node);
  }

  return true;
}

bool ImplicitDepLoader::LoadDepsFromLog(Edge* edge, string* err) {
  // NOTE: deps are only supported for single-target edges.
  Node* output = edge->outputs_[0];
  DepsLog::Deps* deps = deps_log_->GetDeps(output);
  if (!deps) {
    EXPLAIN("deps for '%s' are missing", output->path().c_str());
    return false;
  }

  // Deps are invalid if the output is newer than the deps.
  if (output->mtime() > deps->mtime) {
    EXPLAIN("stored deps info out of date for for '%s' (%d vs %d)",
            output->path().c_str(), deps->mtime, output->mtime());
    return false;
  }

  vector<Node*>::iterator implicit_dep =
      PreallocateSpace(edge, deps->node_count);
  for (int i = 0; i < deps->node_count; ++i, ++implicit_dep) {
    Node* node = deps->nodes[i];
    *implicit_dep = node;
    node->AddOutEdge(edge);
    CreatePhonyInEdge(node);
  }
  return true;
}

vector<Node*>::iterator ImplicitDepLoader::PreallocateSpace(Edge* edge,
                                                            int count) {
  edge->inputs_.insert(edge->inputs_.end() - edge->order_only_deps_,
                       (size_t)count, 0);
  edge->implicit_deps_ += count;
  return edge->inputs_.end() - edge->order_only_deps_ - count;
}

void ImplicitDepLoader::CreatePhonyInEdge(Node* node) {
  if (node->in_edge())
    return;

  Edge* phony_edge = state_->AddEdge(&State::kPhonyRule);
  node->set_in_edge(phony_edge);
  phony_edge->outputs_.push_back(node);

  // RecomputeDirty might not be called for phony_edge if a previous call
  // to RecomputeDirty had caused the file to be stat'ed.  Because previous
  // invocations of RecomputeDirty would have seen this node without an
  // input edge (and therefore ready), we have to set outputs_ready_ to true
  // to avoid a potential stuck build.  If we do call RecomputeDirty for
  // this node, it will simply set outputs_ready_ to the correct value.
  phony_edge->outputs_ready_ = true;
}

bool DependencyScan::RecomputeDirty(Edge* edge, string* err) {
  bool dirty = false;
  edge->outputs_ready_ = true;
  edge->deps_missing_ = false;

  if (!dep_loader_.LoadDeps(edge, err)) {
    if (!err->empty())
      return false;
    // Failed to load dependency info: rebuild to regenerate it.
    dirty = edge->deps_missing_ = true;
  }

  // Visit all inputs; we're dirty if any of the inputs are dirty.
  Node* most_recent_input = NULL;
  for (vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    if ((*i)->StatIfNecessary(disk_interface_)) {
      if (Edge* in_edge = (*i)->in_edge()) {
        if (!RecomputeDirty(in_edge, err))
          return false;
      } else {
        // This input has no in-edge; it is dirty if it is missing.
        if (!(*i)->exists())
          EXPLAIN("%s has no in-edge and is missing", (*i)->path().c_str());
        (*i)->set_dirty(!(*i)->exists());
      }
    }

    // If an input is not ready, neither are our outputs.
    if (Edge* in_edge = (*i)->in_edge()) {
      if (!in_edge->outputs_ready_)
        edge->outputs_ready_ = false;
    }

    if (!edge->is_order_only(i - edge->inputs_.begin())) {
      // If a regular input is dirty (or missing), we're dirty.
      // Otherwise consider mtime.
      if ((*i)->dirty()) {
        EXPLAIN("%s is dirty", (*i)->path().c_str());
        dirty = true;
      } else {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime()) {
          most_recent_input = *i;
        }
      }
    }
  }

  // We may also be dirty due to output state: missing outputs, out of
  // date outputs, etc.  Visit all outputs and determine whether they're dirty.
  if (!dirty)
    dirty = RecomputeOutputsDirty(edge, most_recent_input);

  // Finally, visit each output to mark off that we've visited it, and update
  // their dirty state if necessary.
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    (*o)->StatIfNecessary(disk_interface_);
    if (dirty)
      (*o)->MarkDirty();
  }

  // If an edge is dirty, its outputs are normally not ready.  (It's
  // possible to be clean but still not be ready in the presence of
  // order-only inputs.)
  // But phony edges with no inputs have nothing to do, so are always
  // ready.
  if (dirty && !(edge->is_phony() && edge->inputs_.empty()))
    edge->outputs_ready_ = false;

  return true;
}

bool DependencyScan::RecomputeOutputsDirty(Edge* edge,
                                           Node* most_recent_input) {
  string command = edge->EvaluateCommand(/*incl_rsp_file=*/true);
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    (*o)->StatIfNecessary(disk_interface_);
    if (RecomputeOutputDirty(edge, most_recent_input, command, *o))
      return true;
  }
  return false;
}

bool DependencyScan::RecomputeOutputDirty(Edge* edge,
                                          Node* most_recent_input,
                                          const string& command,
                                          Node* output) {
  if (edge->is_phony()) {
    // Phony edges don't write any output.  Outputs are only dirty if
    // there are no inputs and we're missing the output.
    return edge->inputs_.empty() && !output->exists();
  }

  BuildLog::LogEntry* entry = 0;

  // Dirty if we're missing the output.
  if (!output->exists()) {
    EXPLAIN("output %s doesn't exist", output->path().c_str());
    return true;
  }

  // Dirty if the output is older than the input.
  if (most_recent_input && output->mtime() < most_recent_input->mtime()) {
    TimeStamp output_mtime = output->mtime();

    // If this is a restat rule, we may have cleaned the output with a restat
    // rule in a previous run and stored the most recent input mtime in the
    // build log.  Use that mtime instead, so that the file will only be
    // considered dirty if an input was modified since the previous run.
    bool used_restat = false;
    if (edge->GetBindingBool("restat") && build_log() &&
        (entry = build_log()->LookupByOutput(output->path()))) {
      output_mtime = entry->restat_mtime;
      used_restat = true;
    }

    if (output_mtime < most_recent_input->mtime()) {
      EXPLAIN("%soutput %s older than most recent input %s "
              "(%d vs %d)",
              used_restat ? "restat of " : "", output->path().c_str(),
              most_recent_input->path().c_str(),
              output_mtime, most_recent_input->mtime());
      return true;
    }
  }

  // May also be dirty due to the command changing since the last build.
  // But if this is a generator rule, the command changing does not make us
  // dirty.
  if (!edge->GetBindingBool("generator") && build_log()) {
    if (entry || (entry = build_log()->LookupByOutput(output->path()))) {
      if (BuildLog::LogEntry::HashCommand(command) != entry->command_hash) {
        EXPLAIN("command line changed for %s", output->path().c_str());
        return true;
      }
    }
    if (!entry) {
      EXPLAIN("command line not found in log for %s", output->path().c_str());
      return true;
    }
  }

  return false;
}
