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

#include "manifest_parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "disk_interface.h"
#include "graph.h"
#include "hash_map.h"
#include "state.h"
#include "util.h"
#include "version.h"

using namespace std;

int ManifestParserOptions::default_parse_threads_ = 1;

namespace {

/// Runs tasks on a fixed number of threads.
class TaskRunner {
 public:
  explicit TaskRunner(int num_threads) {
    for (int i = 0; i < num_threads; ++i)
      threads_.emplace_back([this]() { RunTasks(/*until_idle=*/false); });
  }

  ~TaskRunner() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    wakeup_.notify_all();
    for (std::thread& thread : threads_)
      thread.join();
  }

  /// May be called from tasks.
  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
      ++num_unfinished_;
    }
    wakeup_.notify_one();
  }

  /// Runs tasks on the calling thread as well, and returns once all tasks
  /// are done, including the tasks posted by tasks.
  void WaitForAllTasks() { RunTasks(/*until_idle=*/true); }

 private:
  void RunTasks(bool until_idle) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (!tasks_.empty()) {
        std::function<void()> task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        task();
        task = nullptr;
        lock.lock();
        if (--num_unfinished_ == 0)
          wakeup_.notify_all();
        continue;
      }
      if (until_idle ? num_unfinished_ == 0 : done_)
        return;
      wakeup_.wait(lock);
    }
  }

  std::mutex mutex_;
  std::condition_variable wakeup_;
  std::deque<std::function<void()>> tasks_;
  int num_unfinished_ = 0;
  bool done_ = false;
  std::vector<std::thread> threads_;
};

/// A path -> Node map that several threads can use at the same time.
/// State::paths_ can't be used for that, and the order of the nodes in it
/// must not depend on timing either.
class ConcurrentNodeMap {
 public:
  /// Like State::GetNode().
  Node* GetNode(StringPiece path, uint64_t slash_bits) {
    HashedPath key = { path, rapidhash(path.str_, path.len_) };
    // The maps use the low bits of the hash.
    Shard& shard = shards_[key.hash >> (sizeof(size_t) * 8 - kShardBits)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.nodes.find(key);
    if (it != shard.nodes.end())
      return it->second;
    Node* node = new Node(path.AsString(), slash_bits);
    key.path = node->path();
    shard.nodes.insert_unique(key, node);
    return node;
  }

  size_t size() const {
    size_t size = 0;
    for (const Shard& shard : shards_)
      size += shard.nodes.size();
    return size;
  }

  void DeleteAllNodes() {
    for (Shard& shard : shards_) {
      for (auto& entry : shard.nodes)
        delete entry.second;
      shard.nodes.clear();
    }
  }

 private:
  struct HashedPath {
    StringPiece path;
    size_t hash;
    bool operator==(const HashedPath& other) const {
      return path == other.path;
    }
  };
  struct HashedPathHash {
    size_t operator()(const HashedPath& key) const { return key.hash; }
  };
  struct Shard {
    std::mutex mutex;
    emhash8::HashMap<HashedPath, Node*, HashedPathHash> nodes;
  };

  static const int kShardBits = 8;
  Shard shards_[1 << kShardBits];
};

}  // namespace

struct ManifestParser::ParsedFile {
  /// Exactly one of the fields is set.
  struct Item {
    Edge* edge;
    ParsedFile* subninja;
    std::string* default_target;
  };

  ~ParsedFile() {
    // Edges are either owned by the State, or get deleted by
    // DeleteAllEdges().
    for (Item& item : items) {
      delete item.subninja;
      delete item.default_target;
    }
  }

  void DeleteAllEdges() {
    for (Item& item : items) {
      delete item.edge;
      if (item.subninja)
        item.subninja->DeleteAllEdges();
    }
  }

  /// In the order of the statements in the file.
  std::vector<Item> items;
};

struct ManifestParser::ParallelParse {
  explicit ParallelParse(int num_threads)
      // The calling thread parses as well.
      : runner(num_threads - 1) {}

  /// Set if anything happened that parallel parsing doesn't handle.
  std::atomic<bool> gave_up { false };
  /// Set once a file has been handed to another thread.
  std::atomic<bool> has_tasks { false };
  ConcurrentNodeMap nodes;

  std::mutex mutex;  // Protects shared_envs.
  std::vector<BindingEnv*> shared_envs;
  std::vector<Pool*> added_pools;

  /// Last, so that its threads are gone before everything else goes away.
  TaskRunner runner;
};

ManifestParser::ManifestParser(State* state, FileReader* file_reader,
                               ManifestParserOptions options)
    : Parser(state, file_reader),
      options_(options), quiet_(false) {
  env_ = &state->bindings_;
}

std::string ManifestParser::LookupVariable(const std::string& varname) {
  return env_->LookupVariable(varname);
}

bool ManifestParser::Parse(const string& filename, const string& input,
                           string* err) {
  if (!is_subparser_ && !parallel_ && options_.parse_threads_ > 1 &&
      state_->paths_.empty() && state_->edges_.empty() &&
      state_->defaults_.empty()) {
    parsed_in_parallel_ = ParseParallel(filename, input);
    if (parsed_in_parallel_)
      return true;
    // Something came up that ParseParallel() doesn't handle (for example, an
    // error in a manifest). It didn't change state_, so just start over.
  }
  return ParseStatements(filename, input, err);
}

bool ManifestParser::GiveUp() {
  parallel_->gave_up.store(true);
  return false;
}

void ManifestParser::ShareEnv() {
  if (env_is_shared_)
    return;
  env_->BeginConcurrentAccess(&parallel_->gave_up);
  env_is_shared_ = true;
  std::lock_guard<std::mutex> lock(parallel_->mutex);
  parallel_->shared_envs.push_back(env_);
}

bool ManifestParser::ParseParallel(const string& filename,
                                   const string& input) {
  ParallelParse parallel(options_.parse_threads_);
  ParsedFile root;
  parallel_ = &parallel;
  parsed_file_ = &root;
  // Keeps everything that gets added to the root scope separate from what is
  // in there already, so that it can be discarded again.
  ShareEnv();

  string err;
  bool ok = ParseStatements(filename, input, &err);
  parallel.runner.WaitForAllTasks();
  ok = ok && !parallel.gave_up.load();

  parallel_ = nullptr;
  parsed_file_ = nullptr;
  env_is_shared_ = false;

  if (ok) {
    state_->paths_.reserve(parallel.nodes.size());
    ok = CommitParsedFile(root);
  }

  for (BindingEnv* env : parallel.shared_envs)
    env->EndConcurrentAccess(/*keep_additions=*/ok);
  if (ok)
    return true;

  // Undo everything. CommitParsedFile() may have added some of the edges and
  // nodes to state_, and the root file may have added pools.
  state_->edges_.clear();
  state_->paths_.clear();
  state_->defaults_.clear();
  root.DeleteAllEdges();
  parallel.nodes.DeleteAllNodes();
  for (Pool* pool : parallel.added_pools) {
    state_->pools_.erase(pool->name());
    delete pool;
  }
  return false;
}

void ManifestParser::ParseFileTask(const string& path, BindingEnv* parent,
                                   int parent_snapshot,
                                   ParsedFile* parsed_file) {
  if (parallel_->gave_up.load())
    return;
  env_ = new BindingEnv(parent, parent_snapshot);
  parsed_file_ = parsed_file;

  string contents;
  string err;
  if (file_reader_->ReadFile(path, &contents, &err) != FileReader::Okay ||
      !ParseStatements(path, contents, &err)) {
    GiveUp();
  }
}

bool ManifestParser::CommitParsedFile(const ParsedFile& parsed_file) {
  // This does what State::AddEdge(), AddOut(), AddIn() and AddValidation() do
  // when parsing on one thread, in the same order. That includes the order in
  // which nodes get added to State::paths_. Nodes that aren't in there yet
  // are marked by generated_by_dep_loader() still having its initial value.
  State::Paths& paths = state_->paths_;
  auto add_node = [&paths](Node* node) {
    if (node->generated_by_dep_loader()) {
      paths[node->path()] = node;
      node->set_generated_by_dep_loader(false);
    }
  };

  for (const ParsedFile::Item& item : parsed_file.items) {
    if (item.subninja) {
      if (!CommitParsedFile(*item.subninja))
        return false;
      continue;
    }
    if (item.default_target) {
      string err;
      if (!state_->AddDefault(*item.default_target, &err))
        return false;
      continue;
    }

    Edge* edge = item.edge;
    edge->id_ = state_->edges_.size();
    state_->edges_.push_back(edge);
    for (Node* node : edge->outputs_) {
      add_node(node);
      if (node->in_edge())
        return false;  // Several edges (or this edge twice) generate |node|.
      node->set_in_edge(edge);
    }
    for (Node* node : edge->inputs_) {
      add_node(node);
      node->AddOutEdge(edge);
    }
    for (Node* node : edge->validations_) {
      add_node(node);
      node->AddValidationOutEdge(edge);
    }
    if (edge->dyndep_)
      edge->dyndep_->set_dyndep_pending(true);
  }
  return true;
}

bool ManifestParser::ParseStatements(const string& filename,
                                     const string& input, string* err) {
  lexer_.Start(filename, input);

  for (;;) {
    if (parallel_ && parallel_->gave_up.load(std::memory_order_relaxed))
      return false;
    Lexer::Token token = lexer_.ReadToken();
    switch (token) {
    case Lexer::POOL:
      if (!ParsePool(err))
        return false;
      break;
    case Lexer::BUILD:
      if (!ParseEdge(err))
        return false;
      break;
    case Lexer::RULE:
      if (!ParseRule(err))
        return false;
      break;
    case Lexer::DEFAULT:
      if (!ParseDefault(err))
        return false;
      break;
    case Lexer::IDENT: {
      lexer_.UnreadToken();
      string name;
      EvalString let_value;
      if (!ParseLet(&name, &let_value, err))
        return false;
      string value = let_value.Evaluate(env_);
      // Check ninja_required_version immediately so we can exit
      // before encountering any syntactic surprises.
      if (name == "ninja_required_version")
        CheckNinjaVersion(value, &lexer_.manifest_version_major, &lexer_.manifest_version_minor);
      env_->AddBinding(name, value);
      break;
    }
    case Lexer::INCLUDE:
      if (!ParseFileInclude(false, err))
        return false;
      break;
    case Lexer::SUBNINJA:
      if (!ParseFileInclude(true, err))
        return false;
      break;
    case Lexer::ERROR: {
      return lexer_.Error(lexer_.DescribeLastError(), err);
    }
    case Lexer::TEOF:
      return true;
    case Lexer::NEWLINE:
      break;
    default:
      return lexer_.Error(string("unexpected ") + Lexer::TokenName(token),
                          err);
    }
  }
  return false;  // not reached
}


bool ManifestParser::ParsePool(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected pool name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  // Other threads look up pools without synchronization.
  if (parallel_ && parallel_->has_tasks.load())
    return GiveUp();

  if (state_->LookupPool(name) != NULL)
    return lexer_.Error("duplicate pool '" + name + "'", err);

  int depth = -1;

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (key == "depth") {
      string depth_string = value.Evaluate(env_);
      const char* begin = depth_string.data();
      const char* end = begin + depth_string.size();
      auto result = std::from_chars(begin, end, depth);
      if (result.ec != std::errc() || result.ptr != end || depth < 0) {
        return lexer_.Error("invalid pool depth", err);
      }
    } else {
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (depth < 0)
    return lexer_.Error("expected 'depth =' line", err);

  Pool* pool = new Pool(name, depth);
  state_->AddPool(pool);
  if (parallel_)
    parallel_->added_pools.push_back(pool);
  return true;
}


bool ManifestParser::ParseRule(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected rule name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (env_->LookupRuleCurrentScope(name) != NULL)
    return lexer_.Error("duplicate rule '" + name + "'", err);

  auto rule = std::unique_ptr<Rule>(new Rule(name));

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (Rule::IsReservedBinding(key)) {
      rule->AddBinding(key, value);
    } else {
      // Die on other keyvals for now; revisit if we want to add a
      // scope here.
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (rule->bindings_["rspfile"].empty() !=
      rule->bindings_["rspfile_content"].empty()) {
    return lexer_.Error("rspfile and rspfile_content need to be "
                        "both specified", err);
  }

  if (rule->bindings_["command"].empty())
    return lexer_.Error("expected 'command =' line", err);

  env_->AddRule(std::move(rule));
  return true;
}

bool ManifestParser::ParseLet(string* key, EvalString* value, string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);
  if (!ExpectToken(Lexer::EQUALS, err))
    return false;
  if (!lexer_.ReadVarValue(value, err))
    return false;
  return true;
}

bool ManifestParser::ParseDefault(string* err) {
  EvalString eval;
  if (!lexer_.ReadPath(&eval, err))
    return false;
  if (eval.empty())
    return lexer_.Error("expected target name", err);

  do {
    string path = eval.Evaluate(env_);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;  // Unused because this only does lookup.
    CanonicalizePath(&path, &slash_bits);
    if (parallel_) {
      // The target's node isn't in state_ yet, see CommitParsedFile().
      ParsedFile::Item item = { nullptr, nullptr, new string(path) };
      parsed_file_->items.push_back(item);
    } else {
      std::string default_err;
      if (!state_->AddDefault(path, &default_err))
        return lexer_.Error(default_err, err);
    }

    eval.Clear();
    if (!lexer_.ReadPath(&eval, err))
      return false;
  } while (!eval.empty());

  return ExpectToken(Lexer::NEWLINE, err);
}

bool ManifestParser::ReadEdgePaths(vector<EdgePath>* paths, string* err) {
  for (;;) {
    EdgePath path;
    path.eval_index_ = -1;
    if (!lexer_.ReadSimplePath(&path.plain_, &path.is_canonical_)) {
      EvalString eval;
      if (!lexer_.ReadPath(&eval, err))
        return false;
      if (eval.empty())
        return true;
      path.eval_index_ = static_cast<int>(evals_.size());
      evals_.push_back(std::move(eval));
    } else if (path.plain_.empty()) {
      return true;
    }
    paths->push_back(path);
  }
}

bool ManifestParser::EvaluateEdgePath(const EdgePath& edge_path, Env* env,
                                      string* scratch, StringPiece* path,
                                      uint64_t* slash_bits, string* err) {
  if (edge_path.eval_index_ < 0 && edge_path.is_canonical_) {
    *path = edge_path.plain_;
    *slash_bits = 0;
    return true;
  }
  if (edge_path.eval_index_ < 0)
    scratch->assign(edge_path.plain_.str_, edge_path.plain_.len_);
  else
    *scratch = evals_[edge_path.eval_index_].Evaluate(env);
  if (scratch->empty())
    return lexer_.Error("empty path", err);
  CanonicalizePath(scratch, slash_bits);
  *path = *scratch;
  return true;
}

bool ManifestParser::ParseEdge(string* err) {
  ins_.clear();
  outs_.clear();
  validations_.clear();
  evals_.clear();

  if (!ReadEdgePaths(&outs_, err))
    return false;

  // Add all implicit outs, counting how many as we go.
  int implicit_outs = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    size_t explicit_outs = outs_.size();
    if (!ReadEdgePaths(&outs_, err))
      return false;
    implicit_outs = static_cast<int>(outs_.size() - explicit_outs);
  }

  if (outs_.empty())
    return lexer_.Error("expected path", err);

  if (!ExpectToken(Lexer::COLON, err))
    return false;

  string rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return lexer_.Error("expected build command name", err);

  const Rule* rule = env_->LookupRule(rule_name);
  if (!rule)
    return lexer_.Error("unknown build rule '" + rule_name + "'", err);

  // XXX should we require one path here?
  if (!ReadEdgePaths(&ins_, err))
    return false;

  // Add all implicit deps, counting how many as we go.
  int implicit = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    size_t before = ins_.size();
    if (!ReadEdgePaths(&ins_, err))
      return false;
    implicit = static_cast<int>(ins_.size() - before);
  }

  // Add all order-only deps, counting how many as we go.
  int order_only = 0;
  if (lexer_.PeekToken(Lexer::PIPE2)) {
    size_t before = ins_.size();
    if (!ReadEdgePaths(&ins_, err))
      return false;
    order_only = static_cast<int>(ins_.size() - before);
  }

  // Add all validations.
  if (lexer_.PeekToken(Lexer::PIPEAT)) {
    if (!ReadEdgePaths(&validations_, err))
      return false;
  }

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  // Bindings on edges are rare, so allocate per-edge envs only when needed.
  bool has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  BindingEnv* env = has_indent_token ? new BindingEnv(env_) : env_;
  while (has_indent_token) {
    string key;
    EvalString val;
    if (!ParseLet(&key, &val, err))
      return false;

    env->AddBinding(key, val.Evaluate(env_));
    has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  }

  if (parallel_) {
    return ParseEdgeParallel(rule, env, implicit_outs, implicit, order_only,
                             err);
  }

  Edge* edge = state_->AddEdge(rule);
  edge->env_ = env;

  string pool_name = edge->GetBinding("pool");
  if (!pool_name.empty()) {
    Pool* pool = state_->LookupPool(pool_name);
    if (pool == NULL)
      return lexer_.Error("unknown pool name '" + pool_name + "'", err);
    edge->pool_ = pool;
  }

  string scratch;
  StringPiece path;
  uint64_t slash_bits;

  edge->outputs_.reserve(outs_.size());
  for (size_t i = 0, e = outs_.size(); i != e; ++i) {
    if (!EvaluateEdgePath(outs_[i], env, &scratch, &path, &slash_bits, err))
      return false;
    if (!state_->AddOut(edge, path, slash_bits, err)) {
      lexer_.Error(std::string(*err), err);
      return false;
    }
  }

  if (edge->outputs_.empty()) {
    // All outputs of the edge are already created by other edges. Don't add
    // this edge.  Do this check before input nodes are connected to the edge.
    state_->edges_.pop_back();
    delete edge;
    return true;
  }
  edge->implicit_outs_ = implicit_outs;

  edge->inputs_.reserve(ins_.size());
  for (vector<EdgePath>::iterator i = ins_.begin(); i != ins_.end(); ++i) {
    if (!EvaluateEdgePath(*i, env, &scratch, &path, &slash_bits, err))
      return false;
    state_->AddIn(edge, path, slash_bits);
  }
  edge->implicit_deps_ = implicit;
  edge->order_only_deps_ = order_only;

  edge->validations_.reserve(validations_.size());
  for (vector<EdgePath>::iterator v = validations_.begin();
       v != validations_.end(); ++v) {
    if (!EvaluateEdgePath(*v, env, &scratch, &path, &slash_bits, err))
      return false;
    state_->AddValidation(edge, path, slash_bits);
  }

  if (options_.phony_cycle_action_ == kPhonyCycleActionWarn &&
      edge->maybe_phonycycle_diagnostic()) {
    // CMake 2.8.12.x and 3.0.x incorrectly write phony build statements
    // that reference themselves.  Ninja used to tolerate these in the
    // build graph but that has since been fixed.  Filter them out to
    // support users of those old CMake versions.
    Node* out = edge->outputs_[0];
    vector<Node*>::iterator new_end =
        remove(edge->inputs_.begin(), edge->inputs_.end(), out);
    if (new_end != edge->inputs_.end()) {
      out->RemoveOutEdge(edge);
      edge->inputs_.erase(new_end, edge->inputs_.end());
      if (!quiet_) {
        Warning("phony target '%s' names itself as an input; "
                "ignoring [-w phonycycle=warn]",
                out->path().c_str());
      }
    }
  }

  // Lookup, validate, and save any dyndep binding.  It will be used later
  // to load generated dependency information dynamically, but it must
  // be one of our manifest-specified inputs.
  string dyndep = edge->GetUnescapedDyndep();
  if (!dyndep.empty()) {
    uint64_t slash_bits;
    CanonicalizePath(&dyndep, &slash_bits);
    edge->dyndep_ = state_->GetNode(dyndep, slash_bits);
    edge->dyndep_->set_dyndep_pending(true);
    vector<Node*>::iterator dgi =
      std::find(edge->inputs_.begin(), edge->inputs_.end(), edge->dyndep_);
    if (dgi == edge->inputs_.end()) {
      return lexer_.Error("dyndep '" + dyndep + "' is not an input", err);
    }
    assert(!edge->dyndep_->generated_by_dep_loader());
  }

  return true;
}

bool ManifestParser::ParseEdgeParallel(const Rule* rule, BindingEnv* env,
                                       int implicit_outs, int implicit,
                                       int order_only, string* err) {
  // This is the second half of ParseEdge(), except that it doesn't add the
  // edge to state_, see CommitParsedFile(). It gives up instead of reporting
  // errors, ParseEdge() will do that.
  Edge* edge = new Edge;
  edge->rule_ = rule;
  edge->pool_ = &State::kDefaultPool;
  edge->env_ = env;
  ParsedFile::Item item = { edge, nullptr, nullptr };
  parsed_file_->items.push_back(item);

  string pool_name = edge->GetBinding("pool");
  if (!pool_name.empty()) {
    edge->pool_ = state_->LookupPool(pool_name);
    if (edge->pool_ == NULL)
      return GiveUp();
  }

  string scratch;
  StringPiece path;
  uint64_t slash_bits;

  edge->outputs_.reserve(outs_.size());
  for (const EdgePath& out : outs_) {
    if (!EvaluateEdgePath(out, env, &scratch, &path, &slash_bits, err))
      return GiveUp();
    edge->outputs_.push_back(parallel_->nodes.GetNode(path, slash_bits));
  }
  edge->implicit_outs_ = implicit_outs;

  edge->inputs_.reserve(ins_.size());
  for (const EdgePath& in : ins_) {
    if (!EvaluateEdgePath(in, env, &scratch, &path, &slash_bits, err))
      return GiveUp();
    edge->inputs_.push_back(parallel_->nodes.GetNode(path, slash_bits));
  }
  edge->implicit_deps_ = implicit;
  edge->order_only_deps_ = order_only;

  edge->validations_.reserve(validations_.size());
  for (const EdgePath& validation : validations_) {
    if (!EvaluateEdgePath(validation, env, &scratch, &path, &slash_bits, err))
      return GiveUp();
    edge->validations_.push_back(parallel_->nodes.GetNode(path, slash_bits));
  }

  // ParseEdge() prints a warning and removes the input in this case.
  if (options_.phony_cycle_action_ == kPhonyCycleActionWarn &&
      edge->maybe_phonycycle_diagnostic() &&
      std::find(edge->inputs_.begin(), edge->inputs_.end(),
                edge->outputs_[0]) != edge->inputs_.end()) {
    return GiveUp();
  }

  string dyndep = edge->GetUnescapedDyndep();
  if (!dyndep.empty()) {
    CanonicalizePath(&dyndep, &slash_bits);
    edge->dyndep_ = parallel_->nodes.GetNode(dyndep, slash_bits);
    if (std::find(edge->inputs_.begin(), edge->inputs_.end(),
                  edge->dyndep_) == edge->inputs_.end()) {
      return GiveUp();
    }
  }
  return true;
}

bool ManifestParser::ParseFileInclude(bool new_scope, string* err) {
  EvalString eval;
  if (!lexer_.ReadPath(&eval, err))
    return false;
  string path = eval.Evaluate(env_);

  if (parallel_ && new_scope) {
    // Let another thread parse the file, and continue with the current file.
    // Make sure the other thread can read env_ while this thread adds to it.
    ShareEnv();
    ParsedFile* parsed_file = new ParsedFile;
    ParsedFile::Item item = { nullptr, parsed_file, nullptr };
    parsed_file_->items.push_back(item);
    parallel_->has_tasks.store(true);

    BindingEnv* parent_env = env_;
    int parent_snapshot = env_->version();
    ParallelParse* parallel = parallel_;
    State* state = state_;
    FileReader* file_reader = file_reader_;
    ManifestParserOptions options = options_;
    parallel_->runner.Post([=]() {
      ManifestParser parser(state, file_reader, options);
      parser.is_subparser_ = true;
      parser.parallel_ = parallel;
      parser.ParseFileTask(path, parent_env, parent_snapshot, parsed_file);
    });
    return ExpectToken(Lexer::NEWLINE, err);
  }

  if (subparser_ == nullptr) {
    subparser_.reset(new ManifestParser(state_, file_reader_, options_));
    subparser_->is_subparser_ = true;
  }
  if (new_scope) {
    subparser_->env_ = new BindingEnv(env_);
  } else {
    subparser_->env_ = env_;
  }
  // `include` while parsing on several threads: The included file is parsed
  // on this thread as part of the current file.
  subparser_->parallel_ = parallel_;
  subparser_->parsed_file_ = parsed_file_;
  subparser_->env_is_shared_ = new_scope ? false : env_is_shared_;

  bool ok = subparser_->Load(path, err, &lexer_);
  if (!new_scope)
    env_is_shared_ = subparser_->env_is_shared_;
  if (!ok)
    return false;

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  return true;
}
