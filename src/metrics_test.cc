// Copyright 2026 Google Inc. All Rights Reserved.
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

#include "metrics.h"

#include "test.h"

namespace {

struct MetricsTest : public testing::Test {
  virtual void SetUp() { g_metrics = &metrics_; }
  virtual void TearDown() { g_metrics = NULL; }

  Metrics metrics_;
  ScopedTempDir temp_dir_;
};

TEST_F(MetricsTest, Totals) {
  Metric* metric = metrics_.NewMetric("thing");
  for (int i = 0; i < 3; ++i) {
    ScopedMetric scoped(metric);
  }
  EXPECT_EQ(3, metric->count);
  EXPECT_LE(0, metric->sum);
}

TEST_F(MetricsTest, Trace) {
  metrics_.EnableTrace();
  Metric* outer = metrics_.NewMetric("outer");
  Metric* inner = metrics_.NewMetric("inner");
  {
    ScopedMetric scoped_outer(outer);
    ScopedMetric scoped_inner(inner);
  }
  {
    ScopedMetric scoped_inner(inner);
  }

  temp_dir_.CreateAndEnter("MetricsTest-Trace");
  std::string err;
  ASSERT_TRUE(metrics_.WriteTrace("trace.json", &err));
  ASSERT_EQ("", err);

  std::string contents;
  ASSERT_EQ(0, ReadFile("trace.json", &contents, &err));
  // Inner scopes end first.
  EXPECT_EQ("[\n{\"name\":\"inner\",\"ph\":\"X\",\"ts\":", contents.substr(0, 32));
  EXPECT_EQ(3u, std::count(contents.begin(), contents.end(), '{'));
  EXPECT_NE(std::string::npos, contents.find("{\"name\":\"outer\""));
  EXPECT_EQ("}\n]\n", contents.substr(contents.size() - 4));
}

}  // namespace
