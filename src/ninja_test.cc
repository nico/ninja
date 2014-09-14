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

#include "test.h"
#include "line_printer.h"

//static testing::Test* tests[10000];
static testing::Test* (*tests[10000])();
static int ntests;
static LinePrinter printer;

//void RegisterTest(testing::Test* test) {
  //tests[ntests++] = test;
//}
void RegisterTest(testing::Test* (*factory)()) {
  tests[ntests++] = factory;
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

void testing::Test::Check(bool condition, const char* file, int line,
                          const char* error) {
  if (!condition) {
    printer.PrintOnNewLine(
        StringPrintf("*** Failure in %s:%d\n%s\n", file, line, error));
    failed_ = true;
  }
}

int main(int argc, char **argv) {
  int tests_started = 0;

  bool passed = true;
  for (int i = 0; i < 2 /*ntests*/; i++) {
    ++tests_started;

    testing::Test* test = tests[i]();

    printer.Print(
        StringPrintf("[%d/%d] %s", tests_started, ntests, test->Name()),
        LinePrinter::ELIDE);

    test->SetUp();
    test->Run();
    test->TearDown();
    if (test->failed_)
      passed = false;
    delete test;
  }

  printer.PrintOnNewLine(passed ? "passed\n" : "failed\n");
}
