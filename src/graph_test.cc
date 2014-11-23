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

#include "test.h"

struct GraphTest : public StateTestWithBuiltinRules {};

TEST_F(GraphTest, VarInOutPathEscaping) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build a$ b: cat no'space with$ space$$ no\"space2\n"));

  Edge* edge = GetNode("a b")->in_edge();
#if _WIN32
  EXPECT_EQ("cat no'space \"with space$\" \"no\\\"space2\" > \"a b\"",
      edge->EvaluateCommand());
#else
  EXPECT_EQ("cat 'no'\\''space' 'with space$' 'no\"space2' > 'a b'",
      edge->EvaluateCommand());
#endif
}

// Check that rule-level variables are in scope for eval.
TEST_F(GraphTest, RuleVariablesInScope) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule r\n"
"  depfile = x\n"
"  command = depfile is $depfile\n"
"build out: r in\n"));
  Edge* edge = GetNode("out")->in_edge();
  EXPECT_EQ("depfile is x", edge->EvaluateCommand());
}

// Check that build statements can override rule builtins like depfile.
TEST_F(GraphTest, DepfileOverride) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule r\n"
"  depfile = x\n"
"  command = unused\n"
"build out: r in\n"
"  depfile = y\n"));
  Edge* edge = GetNode("out")->in_edge();
  EXPECT_EQ("y", edge->GetBinding("depfile"));
}

// Check that overridden values show up in expansion of rule-level bindings.
TEST_F(GraphTest, DepfileOverrideParent) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule r\n"
"  depfile = x\n"
"  command = depfile is $depfile\n"
"build out: r in\n"
"  depfile = y\n"));
  Edge* edge = GetNode("out")->in_edge();
  EXPECT_EQ("depfile is y", edge->GetBinding("command"));
}

#ifdef _WIN32
TEST_F(GraphTest, Decanonicalize) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out\\out1: cat src\\in1\n"
"build out\\out2/out3\\out4: cat mid1\n"
"build out3 out4\\foo: cat mid1\n"));

  string err;
  vector<Node*> root_nodes = state_.RootNodes(&err);
  EXPECT_EQ(4u, root_nodes.size());
  EXPECT_EQ(root_nodes[0]->path(), "out/out1");
  EXPECT_EQ(root_nodes[1]->path(), "out/out2/out3/out4");
  EXPECT_EQ(root_nodes[2]->path(), "out3");
  EXPECT_EQ(root_nodes[3]->path(), "out4/foo");
  EXPECT_EQ(root_nodes[0]->PathDecanonicalized(), "out\\out1");
  EXPECT_EQ(root_nodes[1]->PathDecanonicalized(), "out\\out2/out3\\out4");
  EXPECT_EQ(root_nodes[2]->PathDecanonicalized(), "out3");
  EXPECT_EQ(root_nodes[3]->PathDecanonicalized(), "out4\\foo");
}
#endif
