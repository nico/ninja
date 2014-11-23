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

#include "scan.h"

#include "build.h"
#include "graph.h"
#include "scan.h"
#include "test.h"

struct ScanTest : public StateTestWithBuiltinRules {
  ScanTest() : scan_(&state_, NULL, NULL, &fs_) {}

  VirtualFileSystem fs_;
  DependencyScan scan_;
};

TEST_F(ScanTest, MissingImplicit) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat in | implicit\n"));
  fs_.Create("in", "");
  fs_.Create("out", "");

  Edge* edge = GetNode("out")->in_edge();
  string err;
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);

  // A missing implicit dep *should* make the output dirty.
  // (In fact, a build will fail.)
  // This is a change from prior semantics of ninja.
  EXPECT_TRUE(GetNode("out")->dirty());
}

TEST_F(ScanTest, ModifiedImplicit) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat in | implicit\n"));
  fs_.Create("in", "");
  fs_.Create("out", "");
  fs_.Tick();
  fs_.Create("implicit", "");

  Edge* edge = GetNode("out")->in_edge();
  string err;
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);

  // A modified implicit dep should make the output dirty.
  EXPECT_TRUE(GetNode("out")->dirty());
}

TEST_F(ScanTest, FunkyMakefilePath) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule catdep\n"
"  depfile = $out.d\n"
"  command = cat $in > $out\n"
"build out.o: catdep foo.cc\n"));
  fs_.Create("foo.cc",  "");
  fs_.Create("out.o.d", "out.o: ./foo/../implicit.h\n");
  fs_.Create("out.o", "");
  fs_.Tick();
  fs_.Create("implicit.h", "");

  Edge* edge = GetNode("out.o")->in_edge();
  string err;
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);

  // implicit.h has changed, though our depfile refers to it with a
  // non-canonical path; we should still find it.
  EXPECT_TRUE(GetNode("out.o")->dirty());
}

TEST_F(ScanTest, ExplicitImplicit) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule catdep\n"
"  depfile = $out.d\n"
"  command = cat $in > $out\n"
"build implicit.h: cat data\n"
"build out.o: catdep foo.cc || implicit.h\n"));
  fs_.Create("implicit.h", "");
  fs_.Create("foo.cc", "");
  fs_.Create("out.o.d", "out.o: implicit.h\n");
  fs_.Create("out.o", "");
  fs_.Tick();
  fs_.Create("data", "");

  Edge* edge = GetNode("out.o")->in_edge();
  string err;
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);

  // We have both an implicit and an explicit dep on implicit.h.
  // The implicit dep should "win" (in the sense that it should cause
  // the output to be dirty).
  EXPECT_TRUE(GetNode("out.o")->dirty());
}

TEST_F(ScanTest, PathWithCurrentDirectory) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule catdep\n"
"  depfile = $out.d\n"
"  command = cat $in > $out\n"
"build ./out.o: catdep ./foo.cc\n"));
  fs_.Create("foo.cc", "");
  fs_.Create("out.o.d", "out.o: foo.cc\n");
  fs_.Create("out.o", "");

  Edge* edge = GetNode("out.o")->in_edge();
  string err;
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);

  EXPECT_FALSE(GetNode("out.o")->dirty());
}

// Regression test for https://github.com/martine/ninja/issues/380
TEST_F(ScanTest, DepfileWithCanonicalizablePath) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule catdep\n"
"  depfile = $out.d\n"
"  command = cat $in > $out\n"
"build ./out.o: catdep ./foo.cc\n"));
  fs_.Create("foo.cc", "");
  fs_.Create("out.o.d", "out.o: bar/../foo.cc\n");
  fs_.Create("out.o", "");

  Edge* edge = GetNode("out.o")->in_edge();
  string err;
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);

  EXPECT_FALSE(GetNode("out.o")->dirty());
}

// Regression test for https://github.com/martine/ninja/issues/404
TEST_F(ScanTest, DepfileRemoved) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule catdep\n"
"  depfile = $out.d\n"
"  command = cat $in > $out\n"
"build ./out.o: catdep ./foo.cc\n"));
  fs_.Create("foo.h", "");
  fs_.Create("foo.cc", "");
  fs_.Tick();
  fs_.Create("out.o.d", "out.o: foo.h\n");
  fs_.Create("out.o", "");

  Edge* edge = GetNode("out.o")->in_edge();
  string err;
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);
  EXPECT_FALSE(GetNode("out.o")->dirty());

  state_.Reset();
  fs_.RemoveFile("out.o.d");
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(GetNode("out.o")->dirty());
}

// Verify that building a nested phony rule prints "no work to do"
TEST_F(ScanTest, NestedPhonyPrintsDone) {
  AssertParse(&state_,
"build n1: phony \n"
"build n2: phony n1\n"
  );
  string err;
  Edge* edge = GetNode("n2")->in_edge();
  EXPECT_TRUE(scan_.RecomputeDirty(edge, &err));
  ASSERT_EQ("", err);

  Plan plan_;
  EXPECT_TRUE(plan_.AddTarget(GetNode("n2"), &err));
  ASSERT_EQ("", err);

  EXPECT_EQ(0, plan_.command_edge_count());
  ASSERT_FALSE(plan_.more_to_do());
}
