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

#include "plan.h"

#include <assert.h>
#include <stdio.h>

#include "graph.h"
#include "scan.h"
#include "state.h"

Plan::Plan() : command_edges_(0), wanted_edges_(0) {}

bool Plan::AddTarget(Node* node, string* err) {
  vector<Node*> stack;
  return AddSubTarget(node, &stack, err);
}

bool Plan::AddSubTarget(Node* node, vector<Node*>* stack, string* err) {
  Edge* edge = node->in_edge();
  if (!edge) {  // Leaf node.
    if (node->dirty()) {
      string referenced;
      if (!stack->empty())
        referenced = ", needed by '" + stack->back()->path() + "',";
      *err = "'" + node->path() + "'" + referenced + " missing "
             "and no known rule to make it";
    }
    return false;
  }

  if (CheckDependencyCycle(node, stack, err))
    return false;

  if (edge->outputs_ready())
    return false;  // Don't need to do anything.

  // If an entry in want_ does not already exist for edge, create an entry which
  // maps to false, indicating that we do not want to build this entry itself.
  pair<map<Edge*, bool>::iterator, bool> want_ins =
    want_.insert(make_pair(edge, false));
  bool& want = want_ins.first->second;

  // If we do need to build edge and we haven't already marked it as wanted,
  // mark it now.
  if (node->dirty() && !want) {
    want = true;
    ++wanted_edges_;
    if (edge->AllInputsReady())
      ScheduleWork(edge);
    if (!edge->is_phony())
      ++command_edges_;
  }

  if (!want_ins.second)
    return true;  // We've already processed the inputs.

  stack->push_back(node);
  for (vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    if (!AddSubTarget(*i, stack, err) && !err->empty())
      return false;
  }
  assert(stack->back() == node);
  stack->pop_back();

  return true;
}

bool Plan::CheckDependencyCycle(Node* node, vector<Node*>* stack, string* err) {
  vector<Node*>::reverse_iterator ri =
      find(stack->rbegin(), stack->rend(), node);
  if (ri == stack->rend())
    return false;

  // Add this node onto the stack to make it clearer where the loop
  // is.
  stack->push_back(node);

  vector<Node*>::iterator start = find(stack->begin(), stack->end(), node);
  *err = "dependency cycle: ";
  for (vector<Node*>::iterator i = start; i != stack->end(); ++i) {
    if (i != start)
      err->append(" -> ");
    err->append((*i)->path());
  }
  return true;
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return NULL;
  set<Edge*>::iterator i = ready_.begin();
  Edge* edge = *i;
  ready_.erase(i);
  return edge;
}

void Plan::ScheduleWork(Edge* edge) {
  Pool* pool = edge->pool();
  if (pool->ShouldDelayEdge()) {
    // The graph is not completely clean. Some Nodes have duplicate Out edges.
    // We need to explicitly ignore these here, otherwise their work will get
    // scheduled twice (see https://github.com/martine/ninja/pull/519)
    if (ready_.count(edge)) {
      return;
    }
    pool->DelayEdge(edge);
    pool->RetrieveReadyEdges(&ready_);
  } else {
    pool->EdgeScheduled(*edge);
    ready_.insert(edge);
  }
}

void Plan::ResumeDelayedJobs(Edge* edge) {
  edge->pool()->EdgeFinished(*edge);
  edge->pool()->RetrieveReadyEdges(&ready_);
}

void Plan::EdgeFinished(Edge* edge) {
  map<Edge*, bool>::iterator i = want_.find(edge);
  assert(i != want_.end());
  if (i->second)
    --wanted_edges_;
  want_.erase(i);
  edge->outputs_ready_ = true;

  // See if this job frees up any delayed jobs
  ResumeDelayedJobs(edge);

  // Check off any nodes we were waiting for with this edge.
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    NodeFinished(*o);
  }
}

void Plan::NodeFinished(Node* node) {
  // See if we we want any edges from this node.
  for (vector<Edge*>::const_iterator i = node->out_edges().begin();
       i != node->out_edges().end(); ++i) {
    map<Edge*, bool>::iterator want_i = want_.find(*i);
    if (want_i == want_.end())
      continue;

    // See if the edge is now ready.
    if ((*i)->AllInputsReady()) {
      if (want_i->second) {
        ScheduleWork(*i);
      } else {
        // We do not need to build this edge, but we might need to build one of
        // its dependents.
        EdgeFinished(*i);
      }
    }
  }
}

void Plan::CleanNode(DependencyScan* scan, Node* node) {
  node->set_dirty(false);

  for (vector<Edge*>::const_iterator ei = node->out_edges().begin();
       ei != node->out_edges().end(); ++ei) {
    // Don't process edges that we don't actually want.
    map<Edge*, bool>::iterator want_i = want_.find(*ei);
    if (want_i == want_.end() || !want_i->second)
      continue;

    // Don't attempt to clean an edge if it failed to load deps.
    if ((*ei)->deps_missing_)
      continue;

    // If all non-order-only inputs for this edge are now clean,
    // we might have changed the dirty state of the outputs.
    vector<Node*>::iterator
        begin = (*ei)->inputs_.begin(),
        end = (*ei)->inputs_.end() - (*ei)->order_only_deps_;
    if (find_if(begin, end, mem_fun(&Node::dirty)) == end) {
      // Recompute most_recent_input.
      Node* most_recent_input = NULL;
      for (vector<Node*>::iterator ni = begin; ni != end; ++ni) {
        if (!most_recent_input || (*ni)->mtime() > most_recent_input->mtime())
          most_recent_input = *ni;
      }

      // Now, this edge is dirty if any of the outputs are dirty.
      // If the edge isn't dirty, clean the outputs and mark the edge as not
      // wanted.
      if (!scan->RecomputeOutputsDirty(*ei, most_recent_input)) {
        for (vector<Node*>::iterator ni = (*ei)->outputs_.begin();
             ni != (*ei)->outputs_.end(); ++ni) {
          CleanNode(scan, *ni);
        }

        want_i->second = false;
        --wanted_edges_;
        if (!(*ei)->is_phony())
          --command_edges_;
      }
    }
  }
}

void Plan::Dump() {
  printf("pending: %d\n", (int)want_.size());
  for (map<Edge*, bool>::iterator i = want_.begin(); i != want_.end(); ++i) {
    if (i->second)
      printf("want ");
    i->first->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}
