// Copyright 2013 Google Inc. All Rights Reserved.
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

#ifndef NINJA_LINE_PRINTER_H_
#define NINJA_LINE_PRINTER_H_

#include <stddef.h>
#include <string>
using namespace std;

/// Prints lines of text, possibly overprinting previously printed lines
/// if the terminal supports it.
struct LinePrinter {
  LinePrinter();

  bool is_smart_terminal() const { return terminal_type_ != TERM_DUMB; }
  void force_dumb_terminal() { terminal_type_ = TERM_DUMB; }

  enum LineType {
    FULL,
    ELIDE
  };
  /// Overprints the current line. If type is ELIDE, elides to_print to fit on
  /// one line.
  void Print(string to_print, LineType type);

  /// Prints a string on a new line, not overprinting previous output.
  void PrintOnNewLine(const string& to_print);

  /// Lock or unlock the console.  Any output sent to the LinePrinter while the
  /// console is locked will not be printed until it is unlocked.
  void SetConsoleLocked(bool locked);

 private:
  /// The type of terminal ninja is writing to
  enum TerminalType {
    /// This terminal doesn't offer any color output or cursor control.
    TERM_DUMB,
#ifdef _WIN32
    /// This terminal is cmd.exe or compatible.  It doesn't understand ANSI
    /// control codes, but it offers colors and cursor control through an
    /// explicit system API.
    TERM_CMD,
#endif
    /// This terminal understands ANSI control codes  -- most interactive
    /// terminals on POSIX, but also cygwin (and similar) on Windows.
    TERM_ANSI
  } terminal_type_;

  /// Whether the caret is at the beginning of a blank line.
  bool have_blank_line_;

  /// Whether console is locked.
  bool console_locked_;

  /// Buffered current line while console is locked.
  string line_buffer_;

  /// Buffered line type while console is locked.
  LineType line_type_;

  /// Buffered console output while console is locked.
  string output_buffer_;

#ifdef _WIN32
  void* console_;
#endif

  /// Print the given data to the console, or buffer it if it is locked.
  void PrintOrBuffer(const char *data, size_t size);
};

#endif  // NINJA_LINE_PRINTER_H_
