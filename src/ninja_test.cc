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

#include <stdlib.h>

#include <gtest/gtest.h>

#include "manifest_parser.h"

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  // Runs all tests that parse manifests with that many threads. The results
  // have to be the same as with one thread.
  if (const char* parse_threads = getenv("NINJA_TEST_PARSE_THREADS"))
    ManifestParserOptions::default_parse_threads_ = atoi(parse_threads);
  // Additionally splits files of at least that many bytes into chunks.
  if (const char* size = getenv("NINJA_TEST_MIN_CHUNKED_FILE_SIZE"))
    ManifestParserOptions::default_min_chunked_file_size_ = atoi(size);
  return RUN_ALL_TESTS();
}
