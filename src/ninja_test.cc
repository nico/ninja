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

#include <stdarg.h>
#include <stdio.h>

#include "gtest/gtest.h"
#include "line_printer.h"

static Test* tests[10000];
static int ntests;

void RegisterTest(Test* test) {
  tests[ntests++] = test;
}

string StringPrintf(const char* format, ...) {
  const int N = 1024;
  char buf[N];

  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, N, format, ap);
  va_end(ap);

  return buf;
}

int main(int argc, char **argv) {
  LinePrinter printer_;
  int tests_started = 0;

  bool passed = true;
  for (int i = 0; i < ntests; i++) {
    ++tests_started;
    printer_.Print(
        StringPrintf("[%d/%d] %s", tests_started, ntests, tests[i]->Name()),
        LinePrinter::ELIDE);

    tests[i]->SetUp();
    if (!tests[i]->Run()) {
      passed = false;
      // XXX: acceptable error messages
      //printer_.PrintOnNewLine(StringPrintf(
      //    "*** Failure in %s:%d\n%s\n", test_part_result.file_name(),
      //    test_part_result.line_number(), test_part_result.summary()));
    }
    tests[i]->TearDown();
  }

  printer_.PrintOnNewLine(passed ? "passed\n" : "failed\n");
}
