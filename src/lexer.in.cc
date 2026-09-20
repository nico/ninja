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

#include <stdio.h>

#include "eval_env.h"
#include "util.h"

using namespace std;

// $^ supported starting this version
const int kMinNewlineEscapeVersionMajor = 1;
const int kMinNewlineEscapeVersionMinor = 14;

bool Lexer::Error(const string& message, string* err) {
  // Compute line/column.
  int line = 1;
  const char* line_start = input_.str_;
  for (const char* p = input_.str_; p < last_token_; ++p) {
    if (*p == '\n') {
      ++line;
      line_start = p + 1;
    }
  }
  int col = last_token_ ? (int)(last_token_ - line_start) : 0;

  char buf[1024];
  snprintf(buf, sizeof(buf), "%s:%d: ", filename_.AsString().c_str(), line);
  *err = buf;
  *err += message + "\n";

  // Add some context to the message.
  const int kTruncateColumn = 72;
  if (col > 0 && col < kTruncateColumn) {
    int len;
    bool truncated = true;
    for (len = 0; len < kTruncateColumn; ++len) {
      if (line_start[len] == 0 || line_start[len] == '\n') {
        truncated = false;
        break;
      }
    }
    *err += string(line_start, len);
    if (truncated)
      *err += "...";
    *err += "\n";
    *err += string(col, ' ');
    *err += "^ near here";
  }

  return false;
}

Lexer::Lexer(const char* input) {
  Start("input", input);
}

void Lexer::Start(StringPiece filename, StringPiece input) {
  filename_ = filename;
  input_ = input;
  ofs_ = input_.str_;
  last_token_ = NULL;
}

const char* Lexer::TokenName(Token t) {
  switch (t) {
  case ERROR:    return "lexing error";
  case BUILD:    return "'build'";
  case COLON:    return "':'";
  case DEFAULT:  return "'default'";
  case EQUALS:   return "'='";
  case IDENT:    return "identifier";
  case INCLUDE:  return "'include'";
  case INDENT:   return "indent";
  case NEWLINE:  return "newline";
  case PIPE2:    return "'||'";
  case PIPE:     return "'|'";
  case PIPEAT:   return "'|@'";
  case POOL:     return "'pool'";
  case RULE:     return "'rule'";
  case SUBNINJA: return "'subninja'";
  case TEOF:     return "eof";
  }
  return NULL;  // not reached
}

const char* Lexer::TokenErrorHint(Token expected) {
  switch (expected) {
  case COLON:
    return " ($ also escapes ':')";
  default:
    return "";
  }
}

string Lexer::DescribeLastError() {
  if (last_token_) {
    switch (last_token_[0]) {
    case '\t':
      return "tabs are not allowed, use spaces";
    }
  }
  return "lexing error";
}

void Lexer::UnreadToken() {
  ofs_ = last_token_;
}

Lexer::Token Lexer::ReadToken() {
  const char* p = ofs_;
  const char* q;
  const char* start;
  Lexer::Token token;
  for (;;) {
    start = p;
    /*!re2c
    re2c:define:YYCTYPE = "unsigned char";
    re2c:define:YYCURSOR = p;
    re2c:define:YYMARKER = q;
    re2c:yyfill:enable = 0;

    nul = "\000";
    simple_varname = [a-zA-Z0-9_-]+;
    varname = [a-zA-Z0-9_.-]+;

    [ ]*"#"[^\000\n]*"\n" { continue; }
    [ ]*"\r\n" { token = NEWLINE;  break; }
    [ ]*"\n"   { token = NEWLINE;  break; }
    [ ]+       { token = INDENT;   break; }
    "build"    { token = BUILD;    break; }
    "pool"     { token = POOL;     break; }
    "rule"     { token = RULE;     break; }
    "default"  { token = DEFAULT;  break; }
    "="        { token = EQUALS;   break; }
    ":"        { token = COLON;    break; }
    "|@"       { token = PIPEAT;   break; }
    "||"       { token = PIPE2;    break; }
    "|"        { token = PIPE;     break; }
    "include"  { token = INCLUDE;  break; }
    "subninja" { token = SUBNINJA; break; }
    varname    { token = IDENT;    break; }
    nul        { token = TEOF;     break; }
    [^]        { token = ERROR;    break; }
    */
  }

  last_token_ = start;
  ofs_ = p;
  if (token != NEWLINE && token != TEOF)
    EatWhitespace();
  return token;
}

bool Lexer::PeekToken(Token token) {
  Token t = ReadToken();
  if (t == token)
    return true;
  UnreadToken();
  return false;
}

void Lexer::EatWhitespace() {
  const char* p = ofs_;
  const char* q;
  for (;;) {
    ofs_ = p;
    /*!re2c
    [ ]+    { continue; }
    "$\r\n" { continue; }
    "$\n"   { continue; }
    nul     { break; }
    [^]     { break; }
    */
  }
}

bool Lexer::ReadIdent(string* out) {
  const char* p = ofs_;
  const char* start;
  for (;;) {
    start = p;
    /*!re2c
    varname {
      out->assign(start, p - start);
      break;
    }
    [^] {
      last_token_ = start;
      return false;
    }
    */
  }
  last_token_ = start;
  ofs_ = p;
  EatWhitespace();
  return true;
}

// ReadSimplePath() is where most of the time for lexing is spent, since most
// of a manifest consists of paths. It can look at 16 bytes at a time.
#if defined(__GNUC__) && defined(__aarch64__)
#define NINJA_LEXER_NEON
#include <arm_neon.h>
#elif defined(__GNUC__) && defined(__SSE2__)
#define NINJA_LEXER_SSE2
#include <emmintrin.h>
#endif

bool Lexer::ReadSimplePath(StringPiece* path, bool* is_canonical) {
  // Bit 0: character ends a simple path, see ReadEvalString().
  // Bit 1: character makes a path non-canonical at the start of a component.
  static const unsigned char kCharClass[256] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0,  // \0 \n \r
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2,  // space $ . /
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,  // :
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,  // |
  };

  const char* start = ofs_;
  const char* p = start;
  // CanonicalizePath() keeps one leading '/' or any number of leading "../".
  // The input is NUL-terminated, so peeking ahead is safe.
  if (*p == '/') {
    ++p;
  } else {
    while (p[0] == '.' && p[1] == '.' && p[2] == '/')
      p += 3;
  }

  // After that, a path is canonical if it has no empty components and no
  // components starting with '.', and if it doesn't end in a slash. This
  // rejects a few canonical paths (e.g. "foo/.bar"), which is fine.
  unsigned non_canonical = 0;
  unsigned component_start = 2;

#if defined(NINJA_LEXER_NEON) || defined(NINJA_LEXER_SSE2)
  // Look at 16 bytes at a time, as long as there are 16 bytes of input left.
  // Computes the same thing as the loop below, which handles the rest.
  const char* const input_end = input_.str_ + input_.len_;
  while (input_end - p >= 16) {
    // Each mask has kBitsPerByte bits per input byte, lowest byte first:
    // |terminators| for bytes with bit 0 set in kCharClass, |bad_starts| for
    // bytes with bit 1 set, |slashes| for '/'.
    uint64_t terminators, bad_starts, slashes;
#if defined(NINJA_LEXER_NEON)
    const int kBitsPerByte = 4;
    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    #define NINJA_EQ(c) vceqq_u8(v, vdupq_n_u8(c))
    #define NINJA_MASK(x) vget_lane_u64(vreinterpret_u64_u8( \
        vshrn_n_u16(vreinterpretq_u16_u8(x), 4)), 0)
    const uint8x16_t slash = NINJA_EQ('/');
    terminators = NINJA_MASK(vorrq_u8(
        vorrq_u8(vorrq_u8(NINJA_EQ('\0'), NINJA_EQ('\n')),
                 vorrq_u8(NINJA_EQ('\r'), NINJA_EQ(' '))),
        vorrq_u8(vorrq_u8(NINJA_EQ('$'), NINJA_EQ(':')), NINJA_EQ('|'))));
#ifdef _WIN32
    // Backslashes need slash_bits, treat them like a bad start everywhere.
    const uint64_t backslashes = NINJA_MASK(NINJA_EQ('\\'));
#endif
    bad_starts = NINJA_MASK(vorrq_u8(slash, NINJA_EQ('.')));
    slashes = NINJA_MASK(slash);
#else
    const int kBitsPerByte = 1;
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    #define NINJA_EQ(c) _mm_cmpeq_epi8(v, _mm_set1_epi8(c))
    #define NINJA_MASK(x) static_cast<uint64_t>(_mm_movemask_epi8(x))
    const __m128i slash = NINJA_EQ('/');
    terminators = NINJA_MASK(_mm_or_si128(
        _mm_or_si128(_mm_or_si128(NINJA_EQ('\0'), NINJA_EQ('\n')),
                     _mm_or_si128(NINJA_EQ('\r'), NINJA_EQ(' '))),
        _mm_or_si128(_mm_or_si128(NINJA_EQ('$'), NINJA_EQ(':')),
                     NINJA_EQ('|'))));
#ifdef _WIN32
    const uint64_t backslashes = NINJA_MASK(NINJA_EQ('\\'));
#endif
    bad_starts = NINJA_MASK(_mm_or_si128(slash, NINJA_EQ('.')));
    slashes = NINJA_MASK(slash);
#endif
    #undef NINJA_EQ
    #undef NINJA_MASK

    // A byte is at the start of a component if the byte before it is a slash
    // (or if it's the first byte and the previous block says so).
    const uint64_t kFirstByte = (uint64_t(1) << kBitsPerByte) - 1;
    uint64_t component_starts =
        (slashes << kBitsPerByte) | (component_start ? kFirstByte : 0);
    uint64_t non_canonicals = bad_starts & component_starts;
#ifdef _WIN32
    non_canonicals |= backslashes;
#endif
    if (terminators) {
      // Only the bytes before the first terminator count.
      int num_bits = __builtin_ctzll(terminators);
      non_canonicals &= (uint64_t(1) << num_bits) - 1;
      if (non_canonicals)
        non_canonical = 2;
      p += num_bits / kBitsPerByte;
      break;
    }
    if (non_canonicals)
      non_canonical = 2;
    component_start = p[15] == '/' ? 2 : 0;
    p += 16;
  }
#endif

  unsigned char c;
  while (!((c = kCharClass[static_cast<unsigned char>(*p)]) & 1)) {
    non_canonical |= c & component_start;
    component_start = *p == '/' ? 2 : 0;
#ifdef _WIN32
    if (*p == '\\')
      non_canonical = 2;  // Needs slash_bits.
#endif
    ++p;
  }
  if (*p == '$' || *p == '\r' || *p == '\0')
    return false;  // Let ReadPath() handle escapes and errors.
  if (p != start && p[-1] == '/')
    non_canonical = 2;

  *path = StringPiece(start, p - start);
  *is_canonical = !non_canonical;
  last_token_ = p;
  ofs_ = p;
  if (p[0] == ' ' && p[1] != ' ' && p[1] != '$')
    ofs_ = p + 1;  // Inlined common case of EatWhitespace().
  else
    EatWhitespace();
  return true;
}

// Returns the first '\n', '\r', '$' or NUL in [p, end), or |end|.
static const char* FindEndOfPlainValue(const char* p, const char* end) {
#if defined(NINJA_LEXER_NEON) || defined(NINJA_LEXER_SSE2)
  while (end - p >= 16) {
#if defined(NINJA_LEXER_NEON)
    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
    const uint8x16_t special = vorrq_u8(
        vorrq_u8(vceqq_u8(v, vdupq_n_u8('\n')), vceqq_u8(v, vdupq_n_u8('$'))),
        vorrq_u8(vceqq_u8(v, vdupq_n_u8('\r')), vceqq_u8(v, vdupq_n_u8(0))));
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(
        vshrn_n_u16(vreinterpretq_u16_u8(special), 4)), 0);
    if (mask)
      return p + __builtin_ctzll(mask) / 4;
#else
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i special = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\n')),
                     _mm_cmpeq_epi8(v, _mm_set1_epi8('$'))),
        _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('\r')),
                     _mm_cmpeq_epi8(v, _mm_set1_epi8(0))));
    int mask = _mm_movemask_epi8(special);
    if (mask)
      return p + __builtin_ctz(mask);
#endif
    p += 16;
  }
#endif
  for (; p < end; ++p) {
    if (*p == '\n' || *p == '\r' || *p == '$' || *p == '\0')
      break;
  }
  return p;
}

bool Lexer::ReadEvalString(EvalString* eval, bool path, string* err) {
  if (!path) {
    // Most variable values contain no $-escapes. The code below would add
    // them to |eval| in many small pieces (it stops at every space, ':' and
    // '|'), so handle them here: the value is everything up to the newline.
    const char* start = ofs_;
    const char* end = FindEndOfPlainValue(start, input_.str_ + input_.len_);
    if (end < input_.str_ + input_.len_ && *end == '\n') {
      if (end != start)
        eval->AddText(StringPiece(start, end - start));
      last_token_ = end;
      ofs_ = end + 1;
      return true;
    }
  }
  const char* p = ofs_;
  const char* q;
  const char* start;
  for (;;) {
    start = p;
    /*!re2c
    [^$ :\r\n|\000]+ {
      eval->AddText(StringPiece(start, p - start));
      continue;
    }
    "\r\n" {
      if (path)
        p = start;
      break;
    }
    [ :|\n] {
      if (path) {
        p = start;
        break;
      } else {
        if (*start == '\n')
          break;
        eval->AddText(StringPiece(start, 1));
        continue;
      }
    }
    "$$" {
      eval->AddText(StringPiece("$", 1));
      continue;
    }
    "$ " {
      eval->AddText(StringPiece(" ", 1));
      continue;
    }
    "$\r\n"[ ]* {
      continue;
    }
    "$\n"[ ]* {
      continue;
    }
    "${"varname"}" {
      eval->AddSpecial(StringPiece(start + 2, p - start - 3));
      continue;
    }
    "$"simple_varname {
      eval->AddSpecial(StringPiece(start + 1, p - start - 1));
      continue;
    }
    "$:" {
      eval->AddText(StringPiece(":", 1));
      continue;
    }
    "$^" {
      if (!newline_version_checked_)
      {
        if ((manifest_version_major < kMinNewlineEscapeVersionMajor) ||
            (manifest_version_major == kMinNewlineEscapeVersionMajor &&
             manifest_version_minor < kMinNewlineEscapeVersionMinor))
        {
          return Error("using $^ escape requires specifying 'ninja_required_version' with version greater or equal 1.14", err);
        }
        newline_version_checked_ = true;
      }
      eval->AddText(StringPiece("\n", 1));
      continue;
    }
    "$". {
      last_token_ = start;
      return Error("bad $-escape (literal $ must be written as $$)", err);
    }
    nul {
      last_token_ = start;
      return Error("unexpected EOF", err);
    }
    [^] {
      last_token_ = start;
      return Error(DescribeLastError(), err);
    }
    */
  }
  last_token_ = start;
  ofs_ = p;
  if (path)
    EatWhitespace();
  // Non-path strings end in newlines, so there's no whitespace to eat.
  return true;
}
