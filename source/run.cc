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

#include "run.h"

#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#include "command_queue.h"
#include "config.h"
#include "deferred_command.h"
#include "invocation.h"
#include "package_metadata.h"
#include "packages.h"
#include "stage.h"
#include "string_replace.h"

namespace {

// Packages that have been ran.
std::set<std::string> packages;

// Adds a package to run, if it's an application.
bool AddPackageToRun(const std::string& package_to_build) {
  if (packages.contains(package_to_build)) return false;

  packages.insert(package_to_build);
  auto* metadata = GetMetadataForPackage(package_to_build);
  if (metadata == nullptr || !metadata->IsApplication()) return false;

  auto command = std::make_unique<DeferredCommand>();
  command->command =
      (std::stringstream() << std::quoted(metadata->output_path.c_str())).str();
  QueueCommand(Stage::Run, std::move(command));

  return true;
}

// Runs each application package individually.
bool RunEachPackageIndividually() {
  int packages_to_run = 0;
  ForEachInputPackage([&packages_to_run](const std::string& package_path) {
    if (AddPackageToRun(GetPackageNameFromPath(package_path)))
      packages_to_run++;
  });
  if (packages_to_run == 0) {
    std::cerr << "Nothing to run." << std::endl;
    return false;
  }
  return true;
}

// Runs the global run command.
void RunGlobalRunCommand(std::string_view global_run_command) {
  auto command = std::make_unique<DeferredCommand>();
  command->command = std::string(global_run_command);
  ReplacePlaceholdersInString(command->command);
  QueueCommand(Stage::Run, std::move(command));
}

// Adds a package to test, if it contains tests.
bool AddPackageToTest(const std::string& package_to_test) {
  auto* metadata = GetMetadataForPackage(package_to_test);
  if (metadata == nullptr) return false;

  std::filesystem::path test_exec_output_path =
      metadata->temp_directory / (package_to_test + "_test");

  std::stringstream cmd_stream;
  cmd_stream << std::quoted(test_exec_output_path.c_str());
  if (!GetTestArgument().empty()) {
    cmd_stream << " " << std::quoted(GetTestArgument());
  }

  auto command = std::make_unique<DeferredCommand>();
  command->command = cmd_stream.str();
  command->destination_file = test_exec_output_path;
  command->package_id = metadata->package_id;
  command->source_file =
      package_to_test;  // Store the package name in source_file.
  QueueCommand(Stage::Run, std::move(command));
  return true;
}

}  // namespace

bool RunPackages() {
  std::string_view global_run_command = GetGlobalRunCommand();

  if (global_run_command.empty()) {
    return RunEachPackageIndividually();
  } else {
    RunGlobalRunCommand(global_run_command);
  }

  return true;
}

bool RunTests() {
  int tests_to_run = 0;
  ForEachInputPackage([&tests_to_run](const std::string& package_path) {
    if (AddPackageToTest(GetPackageNameFromPath(package_path))) tests_to_run++;
  });
  if (tests_to_run == 0) {
    std::cerr << "No tests to run." << std::endl;
    return false;
  }
  return true;
}
