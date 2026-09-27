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

#include "lexer.h"

#include "eval_env.h"
#include "test.h"
#include "util.h"

using namespace std;

TEST(Lexer, ReadVarValue) {
  Lexer lexer("plain text $var $VaR ${x}\n");
  EvalString eval;
  string err;
  EXPECT_TRUE(lexer.ReadVarValue(&eval, &err));
  EXPECT_EQ("", err);
  EXPECT_EQ("[plain text ][$var][ ][$VaR][ ][$x]",
            eval.Serialize());
}

TEST(Lexer, ReadEvalStringEscapes) {
  Lexer lexer("$ $$ab c$: $\ncde\n");
  EvalString eval;
  string err;
  EXPECT_TRUE(lexer.ReadVarValue(&eval, &err));
  EXPECT_EQ("", err);
  EXPECT_EQ("[ $ab c: cde]",
            eval.Serialize());
}

TEST(Lexer, ReadPathCanonical) {
  // If the lexer says that a path is canonical, CanonicalizePath() must not
  // change it. Check all combinations of some interesting components.
  const char* kComponents[] = { "", ".", "..", "a", ".a", "a.", "..a", "bc" };
  const size_t n = sizeof(kComponents) / sizeof(kComponents[0]);
  int num_canonical = 0;
  for (size_t i = 0; i < n * n * n * n; ++i) {
    string in = string(kComponents[i % n]) + "/" + kComponents[i / n % n] +
                "/" + kComponents[i / n / n % n] + "/" +
                kComponents[i / n / n / n];
    // Also test paths with fewer components and without trailing slashes.
    while (!in.empty()) {
      string input = in + "\n";
      Lexer lexer(input.c_str());
      EvalString eval;
      string err;
      EXPECT_TRUE(lexer.ReadPath(&eval, &err)) << in << ": " << err;
      EXPECT_EQ("[" + in + "]", eval.Serialize());
      if (!eval.canonical_path().empty()) {
        string canonical = in;
        uint64_t slash_bits;
        CanonicalizePath(&canonical, &slash_bits);
        EXPECT_EQ(in, canonical);
        EXPECT_EQ(0u, slash_bits);
        ++num_canonical;
      }
      in.resize(in.size() - 1);
    }
  }
  EXPECT_GT(num_canonical, 100);

  const char* kCanonical[] = { "a/b", "../../a", "/a/b", ".a/..b", "a" };
  for (const char* path : kCanonical) {
    string input = string(path) + "\n";
    Lexer lexer(input.c_str());
    EvalString eval;
    string err;
    EXPECT_TRUE(lexer.ReadPath(&eval, &err)) << path << ": " << err;
    EXPECT_EQ(path, eval.canonical_path().AsString());
  }
  // Escapes, variables, backslashes and paths that need canonicalization.
  const char* kNotCanonical[] = { "a$ b", "a$var", "a/./b", "a//b", "./a",
                                  "a/", "a/..", "/", "..", "a\\b" };
  for (const char* path : kNotCanonical) {
    string input = string(path) + "\n";
    Lexer lexer(input.c_str());
    EvalString eval;
    string err;
    EXPECT_TRUE(lexer.ReadPath(&eval, &err)) << path << ": " << err;
    EXPECT_TRUE(eval.canonical_path().empty()) << path;
  }
}

TEST(Lexer, ReadIdent) {
  Lexer lexer("foo baR baz_123 foo-bar");
  string ident;
  EXPECT_TRUE(lexer.ReadIdent(&ident));
  EXPECT_EQ("foo", ident);
  EXPECT_TRUE(lexer.ReadIdent(&ident));
  EXPECT_EQ("baR", ident);
  EXPECT_TRUE(lexer.ReadIdent(&ident));
  EXPECT_EQ("baz_123", ident);
  EXPECT_TRUE(lexer.ReadIdent(&ident));
  EXPECT_EQ("foo-bar", ident);
}

TEST(Lexer, ReadIdentCurlies) {
  // Verify that ReadIdent includes dots in the name,
  // but in an expansion $bar.dots stops at the dot.
  Lexer lexer("foo.dots $bar.dots ${bar.dots}\n");
  string ident;
  EXPECT_TRUE(lexer.ReadIdent(&ident));
  EXPECT_EQ("foo.dots", ident);

  EvalString eval;
  string err;
  EXPECT_TRUE(lexer.ReadVarValue(&eval, &err));
  EXPECT_EQ("", err);
  EXPECT_EQ("[$bar][.dots ][$bar.dots]",
            eval.Serialize());
}

TEST(Lexer, Error) {
  Lexer lexer("foo$\nbad $");
  EvalString eval;
  string err;
  ASSERT_FALSE(lexer.ReadVarValue(&eval, &err));
  EXPECT_EQ("input:2: bad $-escape (literal $ must be written as $$)\n"
            "bad $\n"
            "    ^ near here"
            , err);
}

TEST(Lexer, CommentEOF) {
  // Verify we don't run off the end of the string when the EOF is
  // mid-comment.
  Lexer lexer("# foo");
  Lexer::Token token = lexer.ReadToken();
  EXPECT_EQ(Lexer::ERROR, token);
}

TEST(Lexer, Tabs) {
  // Verify we print a useful error on a disallowed character.
  Lexer lexer("   \tfoobar");
  Lexer::Token token = lexer.ReadToken();
  EXPECT_EQ(Lexer::INDENT, token);
  token = lexer.ReadToken();
  EXPECT_EQ(Lexer::ERROR, token);
  EXPECT_EQ("tabs are not allowed, use spaces", lexer.DescribeLastError());
}

TEST(Lexer, EscapedNewlines) {
  Lexer lexer("foo$\nbar$^newline foo\n");
  EvalString eval;
  string err;
  EXPECT_FALSE(lexer.ReadVarValue(&eval, &err));
  EXPECT_EQ("input:1: using $^ escape requires specifying 'ninja_required_version' with version greater or equal 1.14\n", err);

  lexer.manifest_version_major = 1;
  lexer.manifest_version_minor = 14;

  eval.Clear();
  err = "";
  EXPECT_TRUE(lexer.ReadVarValue(&eval, &err));
  EXPECT_EQ("", err);
  EXPECT_EQ("[foobar\nnewline foo]", eval.Serialize());
}
