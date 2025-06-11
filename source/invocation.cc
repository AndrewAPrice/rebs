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

#include "invocation.h"

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "invocation_action.h"
#include "optimization_level.h"

namespace {

InvocationAction invocation_action = InvocationAction::Run;
OptimizationLevel optimization_level = OptimizationLevel::Fast;
std::vector<std::string> input_packages;
bool all_known_packages = false;
bool verbose = false;

void PrintHelp() {
  std::cout << R"(Usage:
   rebs [packages] <arguments>

If no package is supplied, the working directory is assumed to be the package. A package can be an absolute path, or a relative path if it starts with '.'. Anything else is interpreted as being looked up via the name of the package.

Package arguments:
  --all - Ignore the packages in input and apply the action to all known packages on the system.

Invocation action arguments:
  --build      - Build but don't run.
  --clean      - Clean the temp files for the packages.
  --deep-clean - Clean all the temp files and any cached repositories.
  --run        - Build and run the packages. (Default)
  --test       - Build and run unit tests for the packages.
  --list       - List all known packages with their names and paths, then exit.

 Optimization levels:
  --debug     - Build with all debug symbols.
  --fast      - Quickly build, with some optimizations enabled.
  --optimized - Build will all optimizations enabled.

 Other arguments:
  --verbose   - Be very verbose about the commands being ran and their output.
  --help      - Print this message.
)";
}

}  // namespace

bool ParseInvocation(int argc, char* argv[]) {
  bool abort = false;

  for (int i = 1; i < argc; i++) {
    std::string argument = argv[i];

    if (argument.size() == 0) continue;
    if (argument[0] == '-') {
      if (argument == "--all") {
        all_known_packages = true;
      } else if (argument == "--verbose") {
        verbose = true;
      } else if (argument == "--build") {
        invocation_action = InvocationAction::Build;
      } else if (argument == "--clean") {
        invocation_action = InvocationAction::Clean;
      } else if (argument == "--debug") {
        optimization_level = OptimizationLevel::Debug;
      } else if (argument == "--deep-clean") {
        invocation_action = InvocationAction::DeepClean;
      } else if (argument == "--fast") {
        optimization_level = OptimizationLevel::Fast;
      } else if (argument == "--help") {
        PrintHelp();
        abort = true;
      } else if (argument == "--optimized") {
        optimization_level = OptimizationLevel::Optimized;
      } else if (argument == "--list") {
        invocation_action = InvocationAction::List;
      } else if (argument == "--run") {
        invocation_action = InvocationAction::Run;
      } else {
        std::cerr << "Unknown argument: " << argument << std::endl;
        abort = true;
      }
    } else {
      input_packages.push_back(argument);
    }
  }

  return !abort;
}

InvocationAction GetInvocationAction() { return invocation_action; }

OptimizationLevel GetOptimizationLevel() { return optimization_level; }

void ForEachRawInputPackage(
    const std::function<void(const std::string&)>& on_each_package) {
  if (input_packages.empty()) {
    on_each_package("");
  } else {
    for (const std::string& package : input_packages) on_each_package(package);
  }
}

bool RunOnAllKnownPackages() { return all_known_packages; }

bool ShouldBeVerbose() { return verbose; }
