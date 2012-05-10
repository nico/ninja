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

#include "build_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "build.h"
#include "graph.h"
#include "metrics.h"
#include "util.h"

// Implementation details:
// Each run's log appends to the log file.
// To load, we run through all log entries in series, throwing away
// older runs.
// Once the number of redundant entries exceeds a threshold, we write
// out a new file and replace the existing one with it.

namespace {

const char kFileSignature[] = "# ninja log v%d\n";
const int kCurrentVersion = 4;

}  // namespace

BuildLog::BuildLog()
  : log_file_(NULL), config_(NULL), needs_recompaction_(false) {}

BuildLog::~BuildLog() {
  Close();
}

bool BuildLog::OpenForWrite(const string& path, string* err) {
  if (config_ && config_->dry_run)
    return true;  // Do nothing, report success.

  if (needs_recompaction_) {
    Close();
    if (!Recompact(path, err))
      return false;
  }

  log_file_ = fopen(path.c_str(), "ab");
  if (!log_file_) {
    *err = strerror(errno);
    return false;
  }
  setvbuf(log_file_, NULL, _IOLBF, BUFSIZ);
  SetCloseOnExec(fileno(log_file_));

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(log_file_, 0, SEEK_END);

  if (ftell(log_file_) == 0) {
    if (fprintf(log_file_, kFileSignature, kCurrentVersion) < 0) {
      *err = strerror(errno);
      return false;
    }
  }

  return true;
}

void BuildLog::RecordCommand(Edge* edge, int start_time, int end_time,
                             TimeStamp restat_mtime) {
  string command = edge->EvaluateCommand(true);
  for (vector<Node*>::iterator out = edge->outputs_.begin();
       out != edge->outputs_.end(); ++out) {
    const string& path = (*out)->path();
    Log::iterator i = log_.find(path);
    LogEntry* log_entry;
    if (i != log_.end()) {
      log_entry = i->second;
    } else {
      log_entry = new LogEntry;
      log_entry->output = path;
      log_.insert(Log::value_type(log_entry->output, log_entry));
    }
    log_entry->command = command;
    log_entry->start_time = start_time;
    log_entry->end_time = end_time;
    log_entry->restat_mtime = restat_mtime;

    if (log_file_)
      WriteEntry(log_file_, *log_entry);
  }
}

void BuildLog::Close() {
  if (log_file_)
    fclose(log_file_);
  log_file_ = NULL;
}

bool BuildLog::Load(const string& path, string* err) {
  METRIC_RECORD(".ninja_log load");
  FILE* file = fopen(path.c_str(), "r");
  if (!file) {
    if (errno == ENOENT)
      return true;
    *err = strerror(errno);
    return false;
  }

  int log_version = 0;
  int unique_entry_count = 0;
  int total_entry_count = 0;

  char buf[256 << 10];
  char* line_start = buf, *line_end = NULL, *buf_end = buf;
  while (1) {

    // Get next line.
    if (line_start >= buf_end || !line_end) {
      // Refill buffer.
      size_t size_read = fread(buf, 1, sizeof(buf), file);
      if (!size_read)
        break;
      line_start = buf;
      buf_end = buf + size_read;
    } else {
      // Advance to next line in buffer.
      line_start = line_end + 1;
    }

    line_end = (char*)memchr(line_start, '\n', buf_end - line_start);
    if (!line_end) {
      // No newline. Move rest of data to start of buffer, fill rest.
      size_t watermark = line_start - buf;
      size_t size_rest = (buf_end - buf) - watermark;
      memmove(buf, line_start, size_rest);

      size_t read = fread(buf + size_rest, 1, sizeof(buf) - size_rest, file);
      buf_end = buf + size_rest + read;
      line_start = buf;
      line_end = (char*)memchr(line_start, '\n', buf_end - line_start);
      // If this is NULL again, give up and move on.
      continue;
    }

    // Process line.
    if (!log_version) {
      log_version = 1;  // Assume by default.
      if (sscanf(buf, kFileSignature, &log_version) > 0)
        continue;
    }

    char field_separator = log_version >= 4 ? '\t' : ' ';

    char* start = line_start;
    char* end = (char*)memchr(start, field_separator, line_end - start);
    if (!end)
      continue;
    *end = 0;

    int start_time = 0, end_time = 0;
    TimeStamp restat_mtime = 0;

    start_time = atoi(start);
    start = end + 1;

    end = (char*)memchr(start, field_separator, line_end - start);
    if (!end)
      continue;
    *end = 0;
    end_time = atoi(start);
    start = end + 1;

    end = (char*)memchr(start, field_separator, line_end - start);
    if (!end)
      continue;
    *end = 0;
    restat_mtime = atol(start);
    start = end + 1;

    end = (char*)memchr(start, field_separator, line_end - start);
    if (!end)
      continue;
    string output = string(start, end - start);

    start = end + 1;
    end = line_end;
    if (!end)
      continue;

    LogEntry* entry;
    Log::iterator i = log_.find(output);
    if (i != log_.end()) {
      entry = i->second;
    } else {
      entry = new LogEntry;
      entry->output = output;
      log_.insert(Log::value_type(entry->output, entry));
      ++unique_entry_count;
    }
    ++total_entry_count;

    entry->start_time = start_time;
    entry->end_time = end_time;
    entry->restat_mtime = restat_mtime;
    entry->command = string(start, end - start);
  }

  // Decide whether it's time to rebuild the log:
  // - if we're upgrading versions
  // - if it's getting large
  int kMinCompactionEntryCount = 100;
  int kCompactionRatio = 3;
  if (log_version < kCurrentVersion) {
    needs_recompaction_ = true;
  } else if (total_entry_count > kMinCompactionEntryCount &&
             total_entry_count > unique_entry_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  fclose(file);

  return true;
}

BuildLog::LogEntry* BuildLog::LookupByOutput(const string& path) {
  Log::iterator i = log_.find(path);
  if (i != log_.end())
    return i->second;
  return NULL;
}

void BuildLog::WriteEntry(FILE* f, const LogEntry& entry) {
  fprintf(f, "%d\t%d\t%ld\t%s\t%s\n",
          entry.start_time, entry.end_time, (long) entry.restat_mtime,
          entry.output.c_str(), entry.command.c_str());
}

bool BuildLog::Recompact(const string& path, string* err) {
  printf("Recompacting log...\n");

  string temp_path = path + ".recompact";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) {
    *err = strerror(errno);
    return false;
  }

  if (fprintf(f, kFileSignature, kCurrentVersion) < 0) {
    *err = strerror(errno);
    fclose(f);
    return false;
  }

  for (Log::iterator i = log_.begin(); i != log_.end(); ++i) {
    WriteEntry(f, *i->second);
  }

  fclose(f);
  if (unlink(path.c_str()) < 0) {
    *err = strerror(errno);
    return false;
  }

  if (rename(temp_path.c_str(), path.c_str()) < 0) {
    *err = strerror(errno);
    return false;
  }

  return true;
}
