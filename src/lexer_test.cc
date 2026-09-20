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

TEST(Lexer, ReadSimplePath) {
  Lexer lexer("out/foo.o ../../a/b.cc   /abs/x a$ b | c $\n  d: e\n");
  StringPiece path;
  bool is_canonical = false;
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_EQ("out/foo.o", path.AsString());
  EXPECT_TRUE(is_canonical);
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_EQ("../../a/b.cc", path.AsString());
  EXPECT_TRUE(is_canonical);
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_EQ("/abs/x", path.AsString());
  EXPECT_TRUE(is_canonical);

  // Paths with $-escapes aren't simple. Nothing is consumed, so ReadPath()
  // can read them.
  EXPECT_FALSE(lexer.ReadSimplePath(&path, &is_canonical));
  EvalString eval;
  string err;
  EXPECT_TRUE(lexer.ReadPath(&eval, &err));
  EXPECT_EQ("[a b]", eval.Serialize());

  // Delimiters produce an empty path, like with ReadPath().
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_TRUE(path.empty());
  EXPECT_EQ(Lexer::PIPE, lexer.ReadToken());

  // A path followed by a line continuation.
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_EQ("c", path.AsString());
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_EQ("d", path.AsString());
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_TRUE(path.empty());
  EXPECT_EQ(Lexer::COLON, lexer.ReadToken());
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_EQ("e", path.AsString());
  EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
  EXPECT_TRUE(path.empty());
  EXPECT_EQ(Lexer::NEWLINE, lexer.ReadToken());
  EXPECT_EQ(Lexer::TEOF, lexer.ReadToken());
}

TEST(Lexer, ReadSimplePathErrorsAreLeftToReadPath) {
  StringPiece path;
  bool is_canonical;
  Lexer eof("foo");
  EXPECT_FALSE(eof.ReadSimplePath(&path, &is_canonical));
  Lexer cr("foo\rbar\n");
  EXPECT_FALSE(cr.ReadSimplePath(&path, &is_canonical));
  Lexer crlf("foo\r\n");
  EXPECT_FALSE(crlf.ReadSimplePath(&path, &is_canonical));
}

TEST(Lexer, ReadSimplePathCanonical) {
  // If ReadSimplePath() claims that a path is canonical, CanonicalizePath()
  // must not change it.
  const char* kComponents[] = { "", ".", "..", "a", ".a", "a.", "..a", "bc" };
  const size_t n = sizeof(kComponents) / sizeof(kComponents[0]);
  int num_canonical = 0;
  for (size_t i = 0; i < n * n * n * n; ++i) {
    string in = string(kComponents[i % n]) + "/" + kComponents[i / n % n] +
                "/" + kComponents[i / n / n % n] + "/" +
                kComponents[i / n / n / n];
    // Also test paths with fewer components and without trailing slashes.
    while (!in.empty()) {
      // Lexer doesn't copy its input.
      string input = in + "\n";
      Lexer lexer(input.c_str());
      StringPiece path;
      bool is_canonical = false;
      EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
      EXPECT_EQ(in, path.AsString());
      if (is_canonical) {
        string canonical = in;
        uint64_t slash_bits;
        CanonicalizePath(&canonical, &slash_bits);
        EXPECT_EQ(in, canonical);
        EXPECT_EQ(0u, slash_bits);
        ++num_canonical;
      }

      // ReadSimplePath() looks at 16 bytes at a time while there's enough
      // input left. Put the path at different offsets after a directory name,
      // so that everything ends up at all positions in these blocks, and
      // compare with the result for the same path at the end of the input,
      // where it's handled byte by byte.
      const size_t kPrefixLengths[] = { 1, 2, 5, 9, 12, 13, 14, 15, 16, 17,
                                        29, 30, 31, 32 };
      for (size_t prefix_length : kPrefixLengths) {
        string long_in = string(prefix_length, 'x') + "/" + in;
        string short_input = long_in + "\n";
        Lexer short_lexer(short_input.c_str());
        bool short_is_canonical = false;
        EXPECT_TRUE(short_lexer.ReadSimplePath(&path, &short_is_canonical));
        EXPECT_EQ(long_in, path.AsString());

        string padded = long_in + " next/path/that/is/longer/than/16/bytes\n";
        Lexer long_lexer(padded.c_str());
        bool long_is_canonical = false;
        EXPECT_TRUE(long_lexer.ReadSimplePath(&path, &long_is_canonical));
        EXPECT_EQ(long_in, path.AsString());
        EXPECT_EQ(short_is_canonical, long_is_canonical) << long_in;
        if (long_is_canonical) {
          string canonical = long_in;
          uint64_t slash_bits;
          CanonicalizePath(&canonical, &slash_bits);
          EXPECT_EQ(long_in, canonical);
        }
        // The next path is read correctly too.
        EXPECT_TRUE(long_lexer.ReadSimplePath(&path, &long_is_canonical));
        EXPECT_EQ("next/path/that/is/longer/than/16/bytes", path.AsString());
        EXPECT_TRUE(long_is_canonical);
      }
      in.resize(in.size() - 1);
    }
  }
  EXPECT_GT(num_canonical, 100);

  // All terminators, at all positions of a block.
  const char kTerminators[] = { ' ', ':', '|', '\n' };
  for (size_t length = 1; length < 40; ++length) {
    for (char terminator : kTerminators) {
      string in = string(length, 'y');
      string input = in + terminator + "rest/of/the/input/with/enough/bytes\n";
      Lexer lexer(input.c_str());
      StringPiece path;
      bool is_canonical = false;
      EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
      EXPECT_EQ(in, path.AsString());
      EXPECT_TRUE(is_canonical);
    }
    // These are left to ReadPath().
    const char* kNotSimple[] = { "$", "\r" };
    for (const char* not_simple : kNotSimple) {
      string input = string(length, 'y') + not_simple +
                     "rest/of/the/input/with/enough/bytes\n";
      Lexer lexer(input.c_str());
      StringPiece path;
      bool is_canonical = false;
      EXPECT_FALSE(lexer.ReadSimplePath(&path, &is_canonical));
    }
  }

  Lexer lexer("a/b a/b/ a//b ./a a/./b a/../b ../../a ../a/../b / .. .\n");
  const bool kExpected[] = { true, false, false, false, false, false, true,
                             false, false, false, false };
  for (size_t i = 0; i < sizeof(kExpected) / sizeof(kExpected[0]); ++i) {
    StringPiece path;
    bool is_canonical = false;
    EXPECT_TRUE(lexer.ReadSimplePath(&path, &is_canonical));
    EXPECT_FALSE(path.empty());
    EXPECT_EQ(kExpected[i], is_canonical);
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
