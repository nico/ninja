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

#include <assert.h>
#include <tmmintrin.h>

inline unsigned CountTrailingZeros_32(uint32_t val) {
  return val ? __builtin_ctz(val) : 32;
}


#if 1
char* memchrSSE2(char* str, int c, size_t n) {
    // Write c as sentinel value after string. Assumes that str[n] is writeable.
    char* start = str;
    char old = start[n];
    start[n] = c;

    __m128i needle16 = _mm_set1_epi8(c);

    // Handle unaligned start.
    ptrdiff_t str_as_int = reinterpret_cast<ptrdiff_t>(str);
    size_t n_unaligned = str_as_int & 15;
    if (n_unaligned > 0) {
        __m128i str16 = *(const __m128i*)(str_as_int & ~15);
        __m128i hits16 = _mm_cmpeq_epi8(str16, needle16);
        unsigned long hit_mask = _mm_movemask_epi8(hits16);
        hit_mask &= 0xFFFFFFFF << n_unaligned;
        if (hit_mask) {
            start[n] = old;
            char* r = str + __builtin_ctz(hit_mask);
            return r < start + n ? r : NULL;
        }
        str += 16 - n_unaligned;
    }

    for (;;) {
        __m128i str16 = *(const __m128i*)&str[0];
        __m128i hits16 = _mm_cmpeq_epi8(str16, needle16);
        unsigned long hit_mask = _mm_movemask_epi8(hits16);
        if (hit_mask) {
            start[n] = old;
            char* r = str + __builtin_ctz(hit_mask);
            return r < start + n ? r : NULL;
        }
        str += 16;
    }
}
#else

// http://labs.cybozu.co.jp/blog/mitsunari/search.cpp
#ifdef _WIN32
	#include <intrin.h>
	#define ALIGN(x) __declspec(align(x))
	#define bsf(x) (_BitScanForward(&x, x), x)
	#define bsr(x) (_BitScanReverse(&x, x), x)
#else
	#include <xmmintrin.h>
	#define ALIGN(x) __attribute__((aligned(x)))
	#define bsf(x) __builtin_ctz(x)
#endif

void* memchrSSE2(const void *ptr, int c, size_t len)
{
	const char *p = reinterpret_cast<const char*>(ptr);
	if (len >= 16) {
		__m128i c16 = _mm_set1_epi8(static_cast<char>(c));
		/* 16 byte alignment */
		size_t ip = reinterpret_cast<size_t>(p);
		size_t n = ip & 15;
		if (n > 0) {
			ip &= ~15;
			__m128i x = *(const __m128i*)ip;
			__m128i a = _mm_cmpeq_epi8(x, c16);
			unsigned long mask = _mm_movemask_epi8(a);
			mask &= 0xffffffffUL << n;
			if (mask) {
				return (void*)(ip + bsf(mask));
			}
			n = 16 - n;
			len -= n;
			p += n;
		}
		while (len >= 32) {
			__m128i x = *(const __m128i*)&p[0];
			__m128i y = *(const __m128i*)&p[16];
			__m128i a = _mm_cmpeq_epi8(x, c16);
			__m128i b = _mm_cmpeq_epi8(y, c16);
			unsigned long mask = (_mm_movemask_epi8(b) << 16) | _mm_movemask_epi8(a);
			if (mask) {
				return (void*)(p + bsf(mask));
			}
			len -= 32;
			p += 32;
		}
	}
	while (len > 0) {
		if (*p == c) return (void*)p;
		p++;
		len--;
	}
	return 0;
}
#endif

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

class LineReader {
 public:
  explicit LineReader(FILE* file)
    : file_(file), buf_end_(buf_), line_start_(buf_), line_end_(NULL) {}

  // Reads a \n-terminated line from the file passed to the constructor.
  // On return, *line_start points to the beginning of the next line, and
  // *line_end points to the \n at the end of the line. If no newline is seen
  // in a fixed buffer size, *line_end is set to NULL. Returns false on EOF.
  bool ReadLine(char** line_start, char** line_end) {
    if (line_start_ >= buf_end_ || !line_end_) {
      // Buffer empty, refill.
      size_t size_read = fread(buf_, 1, sizeof(buf_), file_);
      if (!size_read)
        return false;
      line_start_ = buf_;
      buf_end_ = buf_ + size_read;
    } else {
      // Advance to next line in buffer.
      line_start_ = line_end_ + 1;
    }

    line_end_ = (char*)memchrSSE2(line_start_, '\n', buf_end_ - line_start_);
    if (!line_end_) {
      // No newline. Move rest of data to start of buffer, fill rest.
      size_t already_consumed = line_start_ - buf_;
      size_t size_rest = (buf_end_ - buf_) - already_consumed;
      memmove(buf_, line_start_, size_rest);

      size_t read = fread(buf_ + size_rest, 1, sizeof(buf_) - size_rest, file_);
      buf_end_ = buf_ + size_rest + read;
      line_start_ = buf_;
      line_end_ = (char*)memchrSSE2(line_start_, '\n', buf_end_ - line_start_);
    }

    *line_start = line_start_;
    *line_end = line_end_;
    return true;
  }

 private:
  FILE* file_;
  char buf_[256 << 10];
  char* buf_end_;  // Points one past the last valid byte in |buf_|.

  char* line_start_;
  // Points at the next \n in buf_ after line_start, or NULL.
  char* line_end_;
};

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

  LineReader reader(file);
  char* line_start, *line_end;
  while (reader.ReadLine(&line_start, &line_end)) {
    if (!log_version) {
      log_version = 1;  // Assume by default.
      if (sscanf(line_start, kFileSignature, &log_version) > 0)
        continue;
    }

    // If no newline was found in this chunk, read the next.
    if (!line_end)
      continue;

    char field_separator = log_version >= 4 ? '\t' : ' ';

    char* start = line_start;
    char* end = (char*)memchrSSE2(start, field_separator, line_end - start);
    if (!end)
      continue;
    *end = 0;

    int start_time = 0, end_time = 0;
    TimeStamp restat_mtime = 0;

    start_time = atoi(start);
    start = end + 1;

    end = (char*)memchrSSE2(start, field_separator, line_end - start);
    if (!end)
      continue;
    *end = 0;
    end_time = atoi(start);
    start = end + 1;

    end = (char*)memchrSSE2(start, field_separator, line_end - start);
    if (!end)
      continue;
    *end = 0;
    restat_mtime = atol(start);
    start = end + 1;

    end = (char*)memchrSSE2(start, field_separator, line_end - start);
    if (!end)
      continue;
    string output = string(start, end - start);

    start = end + 1;
    end = line_end;

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
