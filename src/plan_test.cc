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

#include "build.h"

#include "plan.h"
#include "graph.h"
#include "state.h"
#include "test.h"

/// Fixture for tests involving Plan.
// Though Plan doesn't use State, it's useful to have one around
// to create Nodes and Edges.
struct PlanTest : public StateTestWithBuiltinRules {
  Plan plan_;

  /// Because FindWork does not return Edges in any sort of predictable order,
  // provide a means to get available Edges in order and in a format which is
  // easy to write tests around.
  void FindWorkSorted(deque<Edge*>* ret, int count) {
    struct CompareEdgesByOutput {
      static bool cmp(const Edge* a, const Edge* b) {
        return a->outputs_[0]->path() < b->outputs_[0]->path();
      }
    };

    for (int i = 0; i < count; ++i) {
      ASSERT_TRUE(plan_.more_to_do());
      Edge* edge = plan_.FindWork();
      ASSERT_TRUE(edge);
      ret->push_back(edge);
    }
    ASSERT_FALSE(plan_.FindWork());
    sort(ret->begin(), ret->end(), CompareEdgesByOutput::cmp);
  }

  void TestPoolWithDepthOne(const char *test_case);
};

TEST_F(PlanTest, Basic) {
  AssertParse(&state_,
"build out: cat mid\n"
"build mid: cat in\n");
  GetNode("mid")->MarkDirty();
  GetNode("out")->MarkDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_EQ("in",  edge->inputs_[0]->path());
  ASSERT_EQ("mid", edge->outputs_[0]->path());

  ASSERT_FALSE(plan_.FindWork());

  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_EQ("mid", edge->inputs_[0]->path());
  ASSERT_EQ("out", edge->outputs_[0]->path());

  plan_.EdgeFinished(edge);

  ASSERT_FALSE(plan_.more_to_do());
  edge = plan_.FindWork();
  ASSERT_EQ(0, edge);
}

// Test that two outputs from one rule can be handled as inputs to the next.
TEST_F(PlanTest, DoubleOutputDirect) {
  AssertParse(&state_,
"build out: cat mid1 mid2\n"
"build mid1 mid2: cat in\n");
  GetNode("mid1")->MarkDirty();
  GetNode("mid2")->MarkDirty();
  GetNode("out")->MarkDirty();

  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge;
  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat in
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat mid1 mid2
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_FALSE(edge);  // done
}

// Test that two outputs from one rule can eventually be routed to another.
TEST_F(PlanTest, DoubleOutputIndirect) {
  AssertParse(&state_,
"build out: cat b1 b2\n"
"build b1: cat a1\n"
"build b2: cat a2\n"
"build a1 a2: cat in\n");
  GetNode("a1")->MarkDirty();
  GetNode("a2")->MarkDirty();
  GetNode("b1")->MarkDirty();
  GetNode("b2")->MarkDirty();
  GetNode("out")->MarkDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge;
  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat in
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat a1
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat a2
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat b1 b2
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_FALSE(edge);  // done
}

// Test that two edges from one output can both execute.
TEST_F(PlanTest, DoubleDependent) {
  AssertParse(&state_,
"build out: cat a1 a2\n"
"build a1: cat mid\n"
"build a2: cat mid\n"
"build mid: cat in\n");
  GetNode("mid")->MarkDirty();
  GetNode("a1")->MarkDirty();
  GetNode("a2")->MarkDirty();
  GetNode("out")->MarkDirty();

  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge;
  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat in
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat mid
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat mid
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat a1 a2
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_FALSE(edge);  // done
}

TEST_F(PlanTest, DependencyCycle) {
  AssertParse(&state_,
"build out: cat mid\n"
"build mid: cat in\n"
"build in: cat pre\n"
"build pre: cat out\n");
  GetNode("out")->MarkDirty();
  GetNode("mid")->MarkDirty();
  GetNode("in")->MarkDirty();
  GetNode("pre")->MarkDirty();

  string err;
  EXPECT_FALSE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("dependency cycle: out -> mid -> in -> pre -> out", err);
}

void PlanTest::TestPoolWithDepthOne(const char* test_case) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_, test_case));
  GetNode("out1")->MarkDirty();
  GetNode("out2")->MarkDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out1"), &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(plan_.AddTarget(GetNode("out2"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_EQ("in",  edge->inputs_[0]->path());
  ASSERT_EQ("out1", edge->outputs_[0]->path());

  // This will be false since poolcat is serialized
  ASSERT_FALSE(plan_.FindWork());

  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_EQ("in", edge->inputs_[0]->path());
  ASSERT_EQ("out2", edge->outputs_[0]->path());

  ASSERT_FALSE(plan_.FindWork());

  plan_.EdgeFinished(edge);

  ASSERT_FALSE(plan_.more_to_do());
  edge = plan_.FindWork();
  ASSERT_EQ(0, edge);
}

TEST_F(PlanTest, PoolWithDepthOne) {
  TestPoolWithDepthOne(
"pool foobar\n"
"  depth = 1\n"
"rule poolcat\n"
"  command = cat $in > $out\n"
"  pool = foobar\n"
"build out1: poolcat in\n"
"build out2: poolcat in\n");
}

TEST_F(PlanTest, ConsolePool) {
  TestPoolWithDepthOne(
"rule poolcat\n"
"  command = cat $in > $out\n"
"  pool = console\n"
"build out1: poolcat in\n"
"build out2: poolcat in\n");
}

TEST_F(PlanTest, PoolsWithDepthTwo) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"pool foobar\n"
"  depth = 2\n"
"pool bazbin\n"
"  depth = 2\n"
"rule foocat\n"
"  command = cat $in > $out\n"
"  pool = foobar\n"
"rule bazcat\n"
"  command = cat $in > $out\n"
"  pool = bazbin\n"
"build out1: foocat in\n"
"build out2: foocat in\n"
"build out3: foocat in\n"
"build outb1: bazcat in\n"
"build outb2: bazcat in\n"
"build outb3: bazcat in\n"
"  pool =\n"
"build allTheThings: cat out1 out2 out3 outb1 outb2 outb3\n"
));
  // Mark all the out* nodes dirty
  for (int i = 0; i < 3; ++i) {
    GetNode("out" + string(1, '1' + static_cast<char>(i)))->MarkDirty();
    GetNode("outb" + string(1, '1' + static_cast<char>(i)))->MarkDirty();
  }
  GetNode("allTheThings")->MarkDirty();

  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("allTheThings"), &err));
  ASSERT_EQ("", err);

  deque<Edge*> edges;
  FindWorkSorted(&edges, 5);

  for (int i = 0; i < 4; ++i) {
    Edge *edge = edges[i];
    ASSERT_EQ("in",  edge->inputs_[0]->path());
    string base_name(i < 2 ? "out" : "outb");
    ASSERT_EQ(base_name + string(1, '1' + (i % 2)), edge->outputs_[0]->path());
  }

  // outb3 is exempt because it has an empty pool
  Edge* edge = edges[4];
  ASSERT_TRUE(edge);
  ASSERT_EQ("in",  edge->inputs_[0]->path());
  ASSERT_EQ("outb3", edge->outputs_[0]->path());

  // finish out1
  plan_.EdgeFinished(edges.front());
  edges.pop_front();

  // out3 should be available
  Edge* out3 = plan_.FindWork();
  ASSERT_TRUE(out3);
  ASSERT_EQ("in",  out3->inputs_[0]->path());
  ASSERT_EQ("out3", out3->outputs_[0]->path());

  ASSERT_FALSE(plan_.FindWork());

  plan_.EdgeFinished(out3);

  ASSERT_FALSE(plan_.FindWork());

  for (deque<Edge*>::iterator it = edges.begin(); it != edges.end(); ++it) {
    plan_.EdgeFinished(*it);
  }

  Edge* last = plan_.FindWork();
  ASSERT_TRUE(last);
  ASSERT_EQ("allTheThings", last->outputs_[0]->path());

  plan_.EdgeFinished(last);

  ASSERT_FALSE(plan_.more_to_do());
  ASSERT_FALSE(plan_.FindWork());
}

TEST_F(PlanTest, PoolWithRedundantEdges) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
    "pool compile\n"
    "  depth = 1\n"
    "rule gen_foo\n"
    "  command = touch foo.cpp\n"
    "rule gen_bar\n"
    "  command = touch bar.cpp\n"
    "rule echo\n"
    "  command = echo $out > $out\n"
    "build foo.cpp.obj: echo foo.cpp || foo.cpp\n"
    "  pool = compile\n"
    "build bar.cpp.obj: echo bar.cpp || bar.cpp\n"
    "  pool = compile\n"
    "build libfoo.a: echo foo.cpp.obj bar.cpp.obj\n"
    "build foo.cpp: gen_foo\n"
    "build bar.cpp: gen_bar\n"
    "build all: phony libfoo.a\n"));
  GetNode("foo.cpp")->MarkDirty();
  GetNode("foo.cpp.obj")->MarkDirty();
  GetNode("bar.cpp")->MarkDirty();
  GetNode("bar.cpp.obj")->MarkDirty();
  GetNode("libfoo.a")->MarkDirty();
  GetNode("all")->MarkDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("all"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge = NULL;

  deque<Edge*> initial_edges;
  FindWorkSorted(&initial_edges, 2);

  edge = initial_edges[1];  // Foo first
  ASSERT_EQ("foo.cpp", edge->outputs_[0]->path());
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_FALSE(plan_.FindWork());
  ASSERT_EQ("foo.cpp", edge->inputs_[0]->path());
  ASSERT_EQ("foo.cpp", edge->inputs_[1]->path());
  ASSERT_EQ("foo.cpp.obj", edge->outputs_[0]->path());
  plan_.EdgeFinished(edge);

  edge = initial_edges[0];  // Now for bar
  ASSERT_EQ("bar.cpp", edge->outputs_[0]->path());
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_FALSE(plan_.FindWork());
  ASSERT_EQ("bar.cpp", edge->inputs_[0]->path());
  ASSERT_EQ("bar.cpp", edge->inputs_[1]->path());
  ASSERT_EQ("bar.cpp.obj", edge->outputs_[0]->path());
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_FALSE(plan_.FindWork());
  ASSERT_EQ("foo.cpp.obj", edge->inputs_[0]->path());
  ASSERT_EQ("bar.cpp.obj", edge->inputs_[1]->path());
  ASSERT_EQ("libfoo.a", edge->outputs_[0]->path());
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_FALSE(plan_.FindWork());
  ASSERT_EQ("libfoo.a", edge->inputs_[0]->path());
  ASSERT_EQ("all", edge->outputs_[0]->path());
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_FALSE(edge);
  ASSERT_FALSE(plan_.more_to_do());
}
