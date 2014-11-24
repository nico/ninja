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
  set<Edge*>::iterator e = ready_.begin();
  Edge* edge = *e;
  ready_.erase(e);
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
  map<Edge*, bool>::iterator e = want_.find(edge);
  assert(e != want_.end());
  if (e->second)
    --wanted_edges_;
  want_.erase(e);
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
  for (vector<Edge*>::const_iterator oe = node->out_edges().begin();
       oe != node->out_edges().end(); ++oe) {
    map<Edge*, bool>::iterator want_i = want_.find(*oe);
    if (want_i == want_.end())
      continue;

    // See if the edge is now ready.
    if ((*oe)->AllInputsReady()) {
      if (want_i->second) {
        ScheduleWork(*oe);
      } else {
        // We do not need to build this edge, but we might need to build one of
        // its dependents.
        EdgeFinished(*oe);
      }
    }
  }
}

void Plan::CleanNode(DependencyScan* scan, Node* node) {
  node->set_dirty(false);

  for (vector<Edge*>::const_iterator oe = node->out_edges().begin();
       oe != node->out_edges().end(); ++oe) {
    // Don't process edges that we don't actually want.
    map<Edge*, bool>::iterator want_i = want_.find(*oe);
    if (want_i == want_.end() || !want_i->second)
      continue;

    // Don't attempt to clean an edge if it failed to load deps.
    if ((*oe)->deps_missing_)
      continue;

    // If all non-order-only inputs for this edge are now clean,
    // we might have changed the dirty state of the outputs.
    vector<Node*>::iterator
        begin = (*oe)->inputs_.begin(),
        end = (*oe)->inputs_.end() - (*oe)->order_only_deps_;
    if (find_if(begin, end, mem_fun(&Node::dirty)) != end)
      continue;

    // Recompute most_recent_input.
    Node* most_recent_input = NULL;
    for (vector<Node*>::iterator i = begin; i != end; ++i) {
      if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime())
        most_recent_input = *i;
    }

    // Now, this edge is dirty if any of the outputs are dirty.
    // If the edge isn't dirty, clean the outputs and mark the edge as not
    // wanted.
    if (!scan->RecomputeOutputsDirty(*oe, most_recent_input)) {
      for (vector<Node*>::iterator o = (*oe)->outputs_.begin();
           o != (*oe)->outputs_.end(); ++o) {
        CleanNode(scan, *o);
      }

      want_i->second = false;
      --wanted_edges_;
      if (!(*oe)->is_phony())
        --command_edges_;
    }
  }
}

void Plan::Dump() {
  printf("pending: %d\n", (int)want_.size());
  for (map<Edge*, bool>::iterator e = want_.begin(); e != want_.end(); ++e) {
    if (e->second)
      printf("want ");
    e->first->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}
