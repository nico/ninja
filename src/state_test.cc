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

#include "state.h"

#include "graph.h"
#include "test.h"

namespace {

TEST(State, Basic) {
  State state;

  EvalString command;
  command.AddText("cat ");
  command.AddSpecial("in");
  command.AddText(" > ");
  command.AddSpecial("out");

  Rule* rule = new Rule("cat");
  rule->AddBinding("command", command);
  state.AddRule(rule);

  Edge* edge = state.AddEdge(rule);
  state.AddIn(edge, "in1", 0);
  state.AddIn(edge, "in2", 0);
  state.AddOut(edge, "out", 0);

  EXPECT_EQ("cat in1 in2 > out", edge->EvaluateCommand());

  EXPECT_FALSE(state.GetNode("in1", 0)->dirty());
  EXPECT_FALSE(state.GetNode("in2", 0)->dirty());
  EXPECT_FALSE(state.GetNode("out", 0)->dirty());
}

struct StateTest : public StateTestWithBuiltinRules {};

TEST_F(StateTest, RootNodes) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out1: cat in1\n"
"build mid1: cat in1\n"
"build out2: cat mid1\n"
"build out3 out4: cat mid1\n"));

  string err;
  vector<Node*> root_nodes = state_.RootNodes(&err);
  EXPECT_EQ(4u, root_nodes.size());
  for (size_t i = 0; i < root_nodes.size(); ++i) {
    string name = root_nodes[i]->path();
    EXPECT_EQ("out", name.substr(0, 3));
  }
}

}  // namespace
