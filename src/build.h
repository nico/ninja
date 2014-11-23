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

#ifndef NINJA_BUILD_H_
#define NINJA_BUILD_H_

#include <cstdio>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "scan.h"
#include "exit_status.h"
#include "line_printer.h"
#include "metrics.h"
#include "plan.h"
#include "util.h"  // int64_t

struct BuildLog;
struct BuildStatus;
struct DepsLog;
struct DiskInterface;
struct Edge;
struct Node;
struct State;

/// CommandRunner is an interface that wraps running the build
/// subcommands.  This allows tests to abstract out running commands.
/// RealCommandRunner is an implementation that actually runs commands.
struct CommandRunner {
  virtual ~CommandRunner() {}
  virtual bool CanRunMore() = 0;
  virtual bool StartCommand(Edge* edge) = 0;

  /// The result of waiting for a command.
  struct Result {
    Result() : edge(NULL) {}
    Edge* edge;
    ExitStatus status;
    string output;
    bool success() const { return status == ExitSuccess; }
  };
  /// Wait for a command to complete, or return false if interrupted.
  virtual bool WaitForCommand(Result* result) = 0;

  virtual vector<Edge*> GetActiveEdges() { return vector<Edge*>(); }
  virtual void Abort() {}
};

/// Options (e.g. verbosity, parallelism) passed to a build.
struct BuildConfig {
  BuildConfig() : verbosity(NORMAL), dry_run(false), parallelism(1),
                  num_failures_allowed(1), max_load_average(-0.0f) {}

  enum Verbosity {
    NORMAL,
    QUIET,  // No output -- used when testing.
    VERBOSE
  };
  Verbosity verbosity;
  bool dry_run;
  int parallelism;
  int num_failures_allowed;
  /// The maximum load average we must not exceed. A negative value
  /// means that we do not have any limit.
  double max_load_average;
};

/// Builder wraps the build process: starting commands, updating status.
struct Builder {
  Builder(State* state, const BuildConfig& config,
          BuildLog* build_log, DepsLog* deps_log,
          DiskInterface* disk_interface);
  ~Builder();

  /// Clean up after interrupted commands by deleting output files.
  void Cleanup();

  Node* AddTarget(const string& name, string* err);

  /// Add a target to the build, scanning dependencies.
  /// @return false on error.
  bool AddTarget(Node* target, string* err);

  /// Returns true if the build targets are already up to date.
  bool AlreadyUpToDate() const;

  /// Run the build.  Returns false on error.
  /// It is an error to call this function when AlreadyUpToDate() is true.
  bool Build(string* err);

  bool StartEdge(Edge* edge, string* err);

  /// Update status ninja logs following a command termination.
  /// @return false if the build can not proceed further due to a fatal error.
  bool FinishCommand(CommandRunner::Result* result, string* err);

  /// Used for tests.
  void SetBuildLog(BuildLog* log) {
    scan_.set_build_log(log);
  }

  State* state_;
  const BuildConfig& config_;
  Plan plan_;
  auto_ptr<CommandRunner> command_runner_;
  BuildStatus* status_;

 private:
   bool ExtractDeps(CommandRunner::Result* result, const string& deps_type,
                    const string& deps_prefix, vector<Node*>* deps_nodes,
                    string* err);

  DiskInterface* disk_interface_;
  DependencyScan scan_;

  // Unimplemented copy ctor and operator= ensure we don't copy the auto_ptr.
  Builder(const Builder &other);        // DO NOT IMPLEMENT
  void operator=(const Builder &other); // DO NOT IMPLEMENT
};

/// Tracks the status of a build: completion fraction, printing updates.
struct BuildStatus {
  explicit BuildStatus(const BuildConfig& config);
  void PlanHasTotalEdges(int total);
  void BuildEdgeStarted(Edge* edge);
  void BuildEdgeFinished(Edge* edge, bool success, const string& output,
                         int* start_time, int* end_time);
  void BuildFinished();

  /// Format the progress status string by replacing the placeholders.
  /// See the user manual for more information about the available
  /// placeholders.
  /// @param progress_status_format The format of the progress status.
  string FormatProgressStatus(const char* progress_status_format) const;

 private:
  void PrintStatus(Edge* edge);

  const BuildConfig& config_;

  /// Time the build started.
  int64_t start_time_millis_;

  int started_edges_, finished_edges_, total_edges_;

  /// Map of running edge to time the edge started running.
  typedef map<Edge*, int> RunningEdgeMap;
  RunningEdgeMap running_edges_;

  /// Prints progress output.
  LinePrinter printer_;

  /// The custom progress status format to use.
  const char* progress_status_format_;

  template<size_t S>
  void snprinfRate(double rate, char(&buf)[S], const char* format) const {
    if (rate == -1) snprintf(buf, S, "?");
    else            snprintf(buf, S, format, rate);
  }

  struct RateInfo {
    RateInfo() : rate_(-1) {}

    void Restart() { stopwatch_.Restart(); }
    double Elapsed() const { return stopwatch_.Elapsed(); }
    double rate() { return rate_; }

    void UpdateRate(int edges) {
      if (edges && stopwatch_.Elapsed())
        rate_ = edges / stopwatch_.Elapsed();
    }

  private:
    double rate_;
    Stopwatch stopwatch_;
  };

  struct SlidingRateInfo {
    SlidingRateInfo(int n) : rate_(-1), N(n), last_update_(-1) {}

    void Restart() { stopwatch_.Restart(); }
    double rate() { return rate_; }

    void UpdateRate(int update_hint) {
      if (update_hint == last_update_)
        return;
      last_update_ = update_hint;

      if (times_.size() == N)
        times_.pop();
      times_.push(stopwatch_.Elapsed());
      if (times_.back() != times_.front())
        rate_ = times_.size() / (times_.back() - times_.front());
    }

  private:
    double rate_;
    Stopwatch stopwatch_;
    const size_t N;
    queue<double> times_;
    int last_update_;
  };

  mutable RateInfo overall_rate_;
  mutable SlidingRateInfo current_rate_;
};

#endif  // NINJA_BUILD_H_
