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

#ifndef NINJA_TEST_H_
#define NINJA_TEST_H_

#include "disk_interface.h"
#include "state.h"
#include "util.h"

// A tiny testing framework inspired by googletest, but much simpler and
// faster to compile.
namespace testing {
struct Test {
  virtual void SetUp() {}
  virtual void TearDown() {}
  virtual bool Run() = 0;
  virtual const char* Name() = 0;

  virtual void Check(bool condition, const char* error);
};
}

void RegisterTest(Test* t);

#define TEST_F_(x, y, name)                      \
  class x##y : public x {                        \
    x##y() { RegisterTest(this, name); }         \
    virtual const char* Name() { return #name; } \
    virtual bool Run();                          \
  };                                             \
  x##y g_instance_##x##y;                        \
  x##y::Run()

#define TEST_F(x, y) TEST_F_(x, y, x##.##y)
#define TEST(x, y) TEST_F_(testing::Test, x##y, x##.##y)

// XXX probably want to include file name, line number, value
#define EXPECT_EQ(a, b) Check(a == b, #a " == " #b)
#define EXPECT_NE(a, b) Check(a != b, #a " != " #b)
#define EXPECT_GT(a, b) Check(a > b, #a " > " #b)
#define EXPECT_TRUE(a) Check(static_cast<bool>(a), #a)
#define EXPECT_FALSE(a) Check(!static_cast<bool(b), #b)

#define ASSERT_EQ(a, b) EXPECT_EQ(a, b); if (a != b) return false
#define ASSERT_NE(a, b) EXPECT_NE(a, b); if (a == b) return false
#define ASSERT_TRUE(a) EXPECT_TRUE(a); if (!static_cast<bool>(a)) return false
#define ASSERT_FALSE(a) EXPECT_FALSE(a) if (static_cast<bool>(a)) return false


// Support utilites for tests.

struct Node;

/// A base test fixture that includes a State object with a
/// builtin "cat" rule.
struct StateTestWithBuiltinRules : public Test {
  StateTestWithBuiltinRules();

  /// Add a "cat" rule to \a state.  Used by some tests; it's
  /// otherwise done by the ctor to state_.
  void AddCatRule(State* state);

  /// Short way to get a Node by its path from state_.
  Node* GetNode(const string& path);

  State state_;
};

void AssertParse(State* state, const char* input);
void AssertHash(const char* expected, uint64_t actual);

/// An implementation of DiskInterface that uses an in-memory representation
/// of disk state.  It also logs file accesses and directory creations
/// so it can be used by tests to verify disk access patterns.
struct VirtualFileSystem : public DiskInterface {
  VirtualFileSystem() : now_(1) {}

  /// "Create" a file with contents.
  void Create(const string& path, const string& contents);

  /// Tick "time" forwards; subsequent file operations will be newer than
  /// previous ones.
  int Tick() {
    return ++now_;
  }

  // DiskInterface
  virtual TimeStamp Stat(const string& path) const;
  virtual bool WriteFile(const string& path, const string& contents);
  virtual bool MakeDir(const string& path);
  virtual string ReadFile(const string& path, string* err);
  virtual int RemoveFile(const string& path);

  /// An entry for a single in-memory file.
  struct Entry {
    int mtime;
    string contents;
  };

  vector<string> directories_made_;
  vector<string> files_read_;
  typedef map<string, Entry> FileMap;
  FileMap files_;
  set<string> files_removed_;
  set<string> files_created_;

  /// A simple fake timestamp for file operations.
  int now_;
};

struct ScopedTempDir {
  /// Create a temporary directory and chdir into it.
  void CreateAndEnter(const string& name);

  /// Clean up the temporary directory.
  void Cleanup();

  /// The temp directory containing our dir.
  string start_dir_;
  /// The subdirectory name for our dir, or empty if it hasn't been set up.
  string temp_dir_name_;
};

#endif // NINJA_TEST_H_
