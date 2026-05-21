// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdio>
#include <functional>
#include <string>

enum class TestResult { Pass, Fail };

// Parses the test output stream byte-by-byte, triggers appropriate event
// callbacks, and returns the complete raw console log (excluding escape
// sequences).
std::string ParseTestOutput(
    FILE* process_output, const std::function<void(int)>& on_total_tests,
    const std::function<void(const std::string&)>& on_each_test_start,
    const std::function<void(const std::string&, TestResult,
                             const std::string&)>& on_test_result);
