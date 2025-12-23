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

#include <filesystem>
#include <iostream>
#include <memory>

#include "build.h"
#include "clangd.h"
#include "command_queue.h"
#include "config.h"
#include "dependencies.h"
#include "invocation.h"
#include "invocation_action.h"
#include "package_id.h"
#include "packages.h"
#include "run.h"
#include "stage.h"
#include "temp_directory.h"

namespace {

// Handles the invocation (clean, build, run, etc.) after everything has been
// initialized.
void ListPackages() {
  std::cout << "All known packages:" << std::endl;
  ForEachKnownPackage([](const std::string& package_path_str) {
    std::filesystem::path package_path(package_path_str);
    std::string package_name = GetPackageNameFromPath(package_path);
    std::cout << " " << package_name << ": " << package_path.string()
              << std::endl;
  });
}

bool HandleInvocation() {
  switch (GetInvocationAction()) {
    case InvocationAction::DeepClean:
      std::cerr << "Deep cleaning is not implement." << std::endl;
      return false;
    case InvocationAction::Clean:
      CleanCurrentConfigurationTempDirectory();
      return true;
    case InvocationAction::Build:
      return BuildPackages();
    case InvocationAction::Run:
      if (!BuildPackages()) return false;
      return RunPackages();
    case InvocationAction::Test:
      std::cerr << "Testing is not implement." << std::endl;
      return false;
    case InvocationAction::List:
      ListPackages();
      return true;
    case InvocationAction::GenerateClangd:
      GenerateClangdForPackages();
      return true;
    default:
      std::cerr << "Unknown invocation." << std::endl;
      return false;
  }
}

// The "main" function that actually does the work, that can return early
// without having to worry about cleaning up. Returns whether the execution is
// successful.
bool WrappedMain() {
  if (!HandleInvocation()) return false;
  if (!RunQueuedCommands()) return false;
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (!ParseInvocation(argc, argv)) return -1;
  InitializeTempDirectory();
  if (!LoadConfigFile()) return -1;
  InitializePackageIDs();
  InitializePackages();

  bool success = WrappedMain();

  FlushDependencies();
  FlushPackageIDs();

  return success ? 0 : -1;
}
