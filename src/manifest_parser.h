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
#include <utility>
#include <vector>

#include "string_piece.h"

struct BindingEnv;
struct Env;
struct EvalString;
struct Node;
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
  /// When parsing with several threads, files at least this big are split
  /// into chunks that are parsed by several threads. This has to be big
  /// enough for that to be worth the overhead.
  size_t min_chunked_file_size_ = default_min_chunked_file_size_;

  /// Tests change these to run all tests with several threads.
  static int default_parse_threads_;
  static size_t default_min_chunked_file_size_;
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
  struct ChunkedFile;
  struct LexedStatement;
  struct ParallelParse;
  struct ParsedFile;

  /// A path in a build statement. Most paths contain no $-escapes; those
  /// are kept as a StringPiece into the lexer's input instead of as an
  /// EvalString, which is much cheaper.
  struct EdgePath {
    StringPiece plain_;   // Only valid if eval_index_ < 0.
    int eval_index_;      // Index into evals_, or -1.
    bool is_canonical_;   // Only valid if eval_index_ < 0.
    Node* node_;          // Set if the path has been resolved already.
  };

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
  bool ParseEdgeParallel(const Rule* rule, BindingEnv* env,
                         const std::vector<EdgePath>& outs,
                         const std::vector<EdgePath>& ins,
                         const std::vector<EdgePath>& validations,
                         const std::vector<EvalString>& evals,
                         int implicit_outs, int implicit, int order_only,
                         std::string* err);
  /// Posts a task that parses the subninja file |path| on another thread.
  void PostSubninjaTask(const std::string& path);
  /// Parses the included file |path| on this thread, in the current scope.
  bool LoadInclude(const std::string& path, bool new_scope, std::string* err);

  /// Parsing a single big file on several threads, as part of ParseParallel().
  /// The file is split into chunks at statement boundaries. Several threads
  /// turn the chunks into LexedStatements. That is most of the work, and it
  /// doesn't depend on scopes. The calling thread then goes through the
  /// statements in order and does everything that does.
  bool ParseStatementsChunked(const std::string& filename,
                              const std::string& input);
  /// Lexes |chunk|, which has to consist of complete statements.
  bool LexChunk(const std::string& filename, const std::string& chunk,
                std::vector<LexedStatement>* statements);
  bool LexBindings(
      std::vector<std::pair<std::string, EvalString> >* bindings);
  bool LexEdgePaths(std::vector<EdgePath>* paths, LexedStatement* statement);
  bool ApplyLexedStatement(LexedStatement* statement);
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

  /// Reads paths into |paths| until there are no more paths.
  bool ReadEdgePaths(std::vector<EdgePath>* paths, std::string* err);

  /// Evaluates and canonicalizes |edge_path|. |path| points either into
  /// the lexer's input or into |scratch|.
  bool EvaluateEdgePath(const EdgePath& edge_path,
                        const std::vector<EvalString>& evals, Env* env,
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
