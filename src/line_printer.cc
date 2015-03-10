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

#include "line_printer.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/time.h>
#endif

#include "util.h"

LinePrinter::LinePrinter() : have_blank_line_(true), console_locked_(false) {
#ifndef _WIN32
  const char* term = getenv("TERM");
  terminal_type_ =
      isatty(1) && term && string(term) != "dumb" ? TERM_ANSI : TERM_DUMB;
#else
  // Disable output buffer.  It'd be nice to use line buffering but
  // MSDN says: "For some systems, [_IOLBF] provides line
  // buffering. However, for Win32, the behavior is the same as _IOFBF
  // - Full Buffering."
  setvbuf(stdout, NULL, _IONBF, 0);
  console_ = GetStdHandle(STD_OUTPUT_HANDLE);

fprintf(stderr, "handle %d\n", console_);
if (console_ == INVALID_HANDLE_VALUE) fprintf(stderr, "INVALID_HANDLE_VALUE\n");
if (GetConsoleWindow() == NULL) fprintf(stderr, "NULL console\n");

  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(console_, &csbi)) {
    terminal_type_ = TERM_CMD;
  } else {
fprintf(stderr, "err %s\n", GetLastErrorString().c_str());
    // ninja is either running in cmd.exe and writing to a pipe, or running in
    // cygwin (or msys, putty, rxvt, ...).  Look at TERM to distinguish between
    // these cases.
    const char* term = getenv("TERM");
    terminal_type_ = term && string(term) != "dumb" ? TERM_ANSI : TERM_DUMB;

    // There's no good way to distinguish "writing to an interactive terminal
    // directly" and "writing to a pipe" in cygwin and friends.  Require
    // $COLUMNS to exist to enable ANSI output.  This seems to be set in
    // interactive sessions but not on bots, so this prevents ANSI codes showing
    // up on bots.  (Note that this is still different from the "right" isatty()
    // check on POSIX: ninja will strip ANSI codes for `ninja | cat` on POSIX
    // because ninja is writing to a pipe, but it won't do that on Windows when
    // not running in cmd.exe.)

fprintf(stderr, "col %s\n", getenv("COLUMNS"));
    if (terminal_type_ == TERM_ANSI && !getenv("COLUMNS"))
      terminal_type_ = TERM_DUMB;
  }
#endif
}

void LinePrinter::Print(string to_print, LineType type) {
  if (console_locked_) {
    line_buffer_ = to_print;
    line_type_ = type;
    return;
  }

  if (is_smart_terminal()) {
    printf("\r");  // Print over previous line, if any.
    // On Windows, calling a C library function writing to stdout also handles
    // pausing the executable when the "Pause" key or Ctrl-S is pressed.
  }

  if (is_smart_terminal() && type == ELIDE) {
    if (terminal_type_ == TERM_ANSI) {
      // Limit output to width of the terminal if provided so we don't cause
      // line-wrapping.
      int term_width = 0;
#ifdef _WIN32
      const char* columns = getenv("COLUMNS");
      if (columns)
        term_width = atoi(columns);
#else
      winsize size;
      if (ioctl(0, TIOCGWINSZ, &size) == 0)
        term_width = size.ws_col;
#endif
      if (term_width)
        to_print = ElideMiddle(to_print, term_width);
      printf("%s", to_print.c_str());
      printf("\x1B[K");  // Clear to end of line.
      fflush(stdout);
    }
#ifdef _WIN32
    else if (terminal_type_ == TERM_CMD) {
      CONSOLE_SCREEN_BUFFER_INFO csbi;
      GetConsoleScreenBufferInfo(console_, &csbi);

      to_print = ElideMiddle(to_print, static_cast<size_t>(csbi.dwSize.X));
      // We don't want to have the cursor spamming back and forth, so instead of
      // printf use WriteConsoleOutput which updates the contents of the buffer,
      // but doesn't move the cursor position.
      COORD buf_size = { csbi.dwSize.X, 1 };
      COORD zero_zero = { 0, 0 };
      SMALL_RECT target = { csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y,
                            static_cast<SHORT>(csbi.dwCursorPosition.X +
                                               csbi.dwSize.X - 1),
                            csbi.dwCursorPosition.Y };
      vector<CHAR_INFO> char_data(csbi.dwSize.X);
      for (size_t i = 0; i < static_cast<size_t>(csbi.dwSize.X); ++i) {
        char_data[i].Char.AsciiChar = i < to_print.size() ? to_print[i] : ' ';
        char_data[i].Attributes = csbi.wAttributes;
      }
      WriteConsoleOutput(console_, &char_data[0], buf_size, zero_zero, &target);
    }
#endif

    have_blank_line_ = false;
  } else {
    printf("%s\n", to_print.c_str());
  }
}

void LinePrinter::PrintOrBuffer(const char* data, size_t size) {
  if (console_locked_) {
    output_buffer_.append(data, size);
  } else {
    // Avoid printf and C strings, since the actual output might contain null
    // bytes like UTF-16 does (yuck).
    fwrite(data, 1, size, stdout);
  }
}

void LinePrinter::PrintOnNewLine(const string& to_print) {
  if (console_locked_ && !line_buffer_.empty()) {
    output_buffer_.append(line_buffer_);
    output_buffer_.append(1, '\n');
    line_buffer_.clear();
  }
  if (!have_blank_line_) {
    PrintOrBuffer("\n", 1);
  }
  if (!to_print.empty()) {
    PrintOrBuffer(&to_print[0], to_print.size());
  }
  have_blank_line_ = to_print.empty() || *to_print.rbegin() == '\n';
}

void LinePrinter::SetConsoleLocked(bool locked) {
  if (locked == console_locked_)
    return;

  if (locked)
    PrintOnNewLine("");

  console_locked_ = locked;

  if (!locked) {
    PrintOnNewLine(output_buffer_);
    if (!line_buffer_.empty()) {
      Print(line_buffer_, line_type_);
    }
    output_buffer_.clear();
    line_buffer_.clear();
  }
}
