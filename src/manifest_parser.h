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

#ifndef NINJA_MANIFEST_PARSER_H_
#define NINJA_MANIFEST_PARSER_H_

#include "parser.h"

#include <memory>
#include <string>
#include <vector>

#include "string_piece.h"

struct BindingEnv;
struct Env;
struct EvalString;
struct Rule;

enum DupeEdgeAction {
  kDupeEdgeActionWarn,
  kDupeEdgeActionError,
};

enum PhonyCycleAction {
  kPhonyCycleActionWarn,
  kPhonyCycleActionError,
};

struct ManifestParserOptions {
  PhonyCycleAction phony_cycle_action_ = kPhonyCycleActionWarn;
  /// Number of threads to parse manifests with. With more than one thread,
  /// files loaded by `subninja` are parsed on other threads. The result is
  /// the same as with one thread.
  int parse_threads_ = default_parse_threads_;

  /// 1, except if tests change it to run with several threads.
  static int default_parse_threads_;
};

/// Parses .ninja files.
struct ManifestParser : public Parser {
  ManifestParser(State* state, FileReader* file_reader,
                 ManifestParserOptions options = ManifestParserOptions());

  /// Parse a text string of input.  Used by tests.
  bool ParseTest(const std::string& input, std::string* err) {
    quiet_ = true;
    return Parse("input", input, err);
  }

  /// Retrieve the expanded value of a top-level variable from the
  /// manifest. Returns an empty string if the variable is not defined.
  /// Must be called only after a successful Load() call.
  std::string LookupVariable(const std::string& varname);

  /// Whether the last Load() or ParseTest() call parsed on several threads.
  /// False if only one thread is allowed, or if parsing had to start over on
  /// a single thread. For tests.
  bool parsed_in_parallel() const { return parsed_in_parallel_; }

 private:
  struct ParallelParse;
  struct ParsedFile;

  /// Parse a file, given its contents as a string.
  bool Parse(const std::string& filename, const std::string& input,
             std::string* err);

  /// Parses all statements in |input|.
  bool ParseStatements(const std::string& filename, const std::string& input,
                       std::string* err);

  /// Parsing on several threads. This is optimistic: it handles what
  /// manifests usually look like, and gives up on everything else (including
  /// all errors). In that case, nothing has been added to state_ and the
  /// manifest needs to be parsed again the normal way.
  ///
  /// Every file is parsed by one thread, but files loaded with `subninja` are
  /// parsed by other threads while the current thread continues with the
  /// current file. While that's happening, nothing gets added to state_, since
  /// the order of edges and nodes in it must not depend on timing. Instead,
  /// edges are collected in a ParsedFile per file. Once all files are parsed,
  /// the edges are added to state_ in the order a single thread would have
  /// added them in.
  /// @return false if the manifest has to be parsed again the normal way.
  bool ParseParallel(const std::string& filename, const std::string& input);
  /// Parses |path| on a thread of parallel_, with a new scope below |parent|.
  void ParseFileTask(const std::string& path, BindingEnv* parent,
                     int parent_snapshot, ParsedFile* parsed_file);
  bool ParseEdgeParallel(const Rule* rule, BindingEnv* env, int implicit_outs,
                         int implicit, int order_only, std::string* err);
  /// Adds everything in |parsed_file| to state_.
  bool CommitParsedFile(const ParsedFile& parsed_file);
  /// Makes env_ safe for use by other threads, if it isn't already.
  void ShareEnv();
  /// For parallel parsing, to give up. Always returns false.
  bool GiveUp();

  /// Parse various statement types.
  bool ParsePool(std::string* err);
  bool ParseRule(std::string* err);
  bool ParseLet(std::string* key, EvalString* val, std::string* err);
  bool ParseEdge(std::string* err);
  bool ParseDefault(std::string* err);

  /// Parse either a 'subninja' or 'include' line.
  bool ParseFileInclude(bool new_scope, std::string* err);

  BindingEnv* env_;
  ManifestParserOptions options_;
  bool quiet_;

  /// Set while parsing on several threads, see ParseParallel().
  ParallelParse* parallel_ = nullptr;
  ParsedFile* parsed_file_ = nullptr;
  bool env_is_shared_ = false;
  bool parsed_in_parallel_ = false;
  /// True for parsers created by other parsers for subninja and include.
  bool is_subparser_ = false;

  /// A path in a build statement. Most paths contain no $-escapes; those
  /// are kept as a StringPiece into the lexer's input instead of as an
  /// EvalString, which is much cheaper.
  struct EdgePath {
    StringPiece plain_;   // Only valid if eval_index_ < 0.
    int eval_index_;      // Index into evals_, or -1.
    bool is_canonical_;   // Only valid if eval_index_ < 0.
  };

  /// Reads paths into |paths| until there are no more paths.
  bool ReadEdgePaths(std::vector<EdgePath>* paths, std::string* err);

  /// Evaluates and canonicalizes |edge_path|. |path| points either into
  /// the lexer's input or into |scratch|.
  bool EvaluateEdgePath(const EdgePath& edge_path, Env* env,
                        std::string* scratch, StringPiece* path,
                        uint64_t* slash_bits, std::string* err);

  // ins_/out_/validations_/evals_ are reused across invocations to ParseEdge(),
  // to save on the otherwise constant memory reallocation.
  // subparser_ is reused solely to get better reuse out ins_/outs_/validation_.
  std::unique_ptr<ManifestParser> subparser_;
  std::vector<EdgePath> ins_, outs_, validations_;
  std::vector<EvalString> evals_;
};

#endif  // NINJA_MANIFEST_PARSER_H_
