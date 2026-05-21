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

#include "test_parsing.h"

#include <cctype>
#include <cstdio>
#include <functional>
#include <string>

std::string ParseTestOutput(
    FILE* process_output, const std::function<void(int)>& on_total_tests,
    const std::function<void(const std::string&)>& on_each_test_start,
    const std::function<void(const std::string&, TestResult,
                             const std::string&)>& on_test_result) {
  enum class ParseState {
    Normal,
    Esc,
    LBracket,
    Question,
    CommandId,
    Argument
  };

  enum class TestStatus { NotStarted, Running, Finished };

  ParseState parse_state = ParseState::Normal;
  std::string cmd_id_buffer;
  std::string arg_buffer;
  std::string suite_log;

  TestStatus current_test_status = TestStatus::NotStarted;
  std::string current_test_name = "Test Startup";
  bool is_current_test_failing = false;
  std::string current_failure_log;

  auto append_char_to_failure_log = [&](char ch) {
    if (current_test_status == TestStatus::Running ||
        current_test_status == TestStatus::NotStarted ||
        is_current_test_failing) {
      current_failure_log += ch;
    }
  };

  auto append_str_to_failure_log = [&](const std::string& s) {
    if (current_test_status == TestStatus::Running ||
        current_test_status == TestStatus::NotStarted ||
        is_current_test_failing) {
      current_failure_log += s;
    }
  };

  auto flush_previous_failure = [&]() {
    if (is_current_test_failing) {
      on_test_result(current_test_name, TestResult::Fail, current_failure_log);
      is_current_test_failing = false;
    }
  };

  int c_int;
  while ((c_int = fgetc(process_output)) != EOF) {
    char c = static_cast<char>(c_int);
    switch (parse_state) {
      case ParseState::Normal:
        if (c == '\x1B') {
          parse_state = ParseState::Esc;
        } else {
          suite_log += c;
          append_char_to_failure_log(c);
        }
        break;
      case ParseState::Esc:
        if (c == '[') {
          parse_state = ParseState::LBracket;
        } else {
          suite_log += '\x1B';
          suite_log += c;
          append_char_to_failure_log('\x1B');
          append_char_to_failure_log(c);
          parse_state = ParseState::Normal;
        }
        break;
      case ParseState::LBracket:
        if (c == '?') {
          parse_state = ParseState::Question;
          cmd_id_buffer.clear();
          arg_buffer.clear();
        } else {
          suite_log += "\x1B[";
          suite_log += c;
          append_str_to_failure_log("\x1B[");
          append_char_to_failure_log(c);
          parse_state = ParseState::Normal;
        }
        break;
      case ParseState::Question:
        if (isdigit(c)) {
          cmd_id_buffer += c;
          parse_state = ParseState::CommandId;
        } else if (c == ';') {
          parse_state = ParseState::Argument;
        } else {
          suite_log += "\x1B[?";
          suite_log += cmd_id_buffer;
          suite_log += c;
          append_str_to_failure_log("\x1B[?");
          append_str_to_failure_log(cmd_id_buffer);
          append_char_to_failure_log(c);
          parse_state = ParseState::Normal;
        }
        break;
      case ParseState::CommandId:
        if (isdigit(c)) {
          cmd_id_buffer += c;
        } else if (c == ';') {
          if (!cmd_id_buffer.empty()) {
            int cmd_id = std::stoi(cmd_id_buffer);
            if (cmd_id == 102) {
              if (current_test_status == TestStatus::Running) {
                current_test_status = TestStatus::Finished;
                on_test_result(current_test_name, TestResult::Pass, "");
                current_failure_log.clear();
              }
              parse_state = ParseState::Normal;
            } else if (cmd_id == 103) {
              if (current_test_status == TestStatus::Running) {
                current_test_status = TestStatus::Finished;
                is_current_test_failing = true;
              }
              parse_state = ParseState::Normal;
            } else {
              parse_state = ParseState::Argument;
            }
          } else {
            parse_state = ParseState::Argument;
          }
        } else {
          suite_log += "\x1B[?";
          suite_log += cmd_id_buffer;
          suite_log += c;
          append_str_to_failure_log("\x1B[?");
          append_str_to_failure_log(cmd_id_buffer);
          append_char_to_failure_log(c);
          parse_state = ParseState::Normal;
        }
        break;
      case ParseState::Argument:
        if (c == 'T' || static_cast<unsigned char>(c) < 32) {
          if (!cmd_id_buffer.empty()) {
            int cmd_id = std::stoi(cmd_id_buffer);
            if (cmd_id == 100) {
              on_total_tests(std::stoi(arg_buffer));
            } else if (cmd_id == 101) {
              if (current_test_status == TestStatus::Running) {
                if (!current_failure_log.empty() &&
                    current_failure_log.back() != '\n') {
                  current_failure_log += "\n";
                }
                current_failure_log +=
                    "Application terminated before test finished.";
                on_test_result(current_test_name, TestResult::Fail,
                               current_failure_log);
              } else {
                flush_previous_failure();
              }
              current_test_name = arg_buffer;
              current_test_status = TestStatus::Running;
              is_current_test_failing = false;
              current_failure_log.clear();
              on_each_test_start(current_test_name);
            }
          }
          if (c == '\x1B') {
            parse_state = ParseState::Esc;
          } else {
            parse_state = ParseState::Normal;
          }
        } else {
          arg_buffer += c;
        }
        break;
    }
  }

  if (current_test_status == TestStatus::Running) {
    if (!current_failure_log.empty() && current_failure_log.back() != '\n') {
      current_failure_log += "\n";
    }
    current_failure_log += "Application terminated before test finished.";
    on_test_result(current_test_name, TestResult::Fail, current_failure_log);
  } else {
    flush_previous_failure();
  }
  return suite_log;
}
