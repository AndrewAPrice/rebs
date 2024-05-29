// Copyright 2024 Google LLC
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

#include "execute.h"

#include <array>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#define WEXITSTATUS
#endif

namespace {

// Returns the output stream, which is the provided stream, if it's not null, or
// stderr.
std::ostream& OutputStream(std::stringstream* opt_output) {
  return opt_output ? *opt_output : std::cerr;
}

}  // namespace

bool ExecuteCommand(const std::string& command, std::stringstream* opt_output) {
  // Redirect stderr to stdout.
  std::string raw_command = command + " 2>&1";

  // Try to run the command and open a pipe to it.
  FILE* pipe = popen(raw_command.c_str(), "r");
  if (pipe == nullptr) {
    OutputStream(opt_output)
        << "Unknown error executing: " << command << std::endl;
    return false;
  }

  // Temporary string to show the output of the program, which may be outputted
  // if the program doesn't successfully run.
  std::string output;

  try {
    // Read the output from the program into `output`.
    std::size_t bytesread;
    std::array<char, 1024> buffer{};
    while ((bytesread = std::fread(buffer.data(), sizeof(buffer.at(0)),
                                   sizeof(buffer), pipe)) != 0) {
      output += std::string(buffer.data(), bytesread);
    }

    // Return if the program closed without error.
    int return_code = pclose(pipe);
    if (WEXITSTATUS(return_code) == EXIT_SUCCESS) return true;
  } catch (...) {
    throw;
  }

  OutputStream(opt_output) << "Error executing: " << command << std::endl;
  if (output.size() > 0) OutputStream(opt_output) << output << std::endl;
  return false;
}
