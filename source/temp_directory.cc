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

#include "temp_directory.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include "config.h"
#include "invocation.h"
#include "optimization_level.h"
#include "package_id.h"
#include "string_replace.h"

namespace {

// The name of the sub directory inside of the system's temporary directory.
constexpr char kTempSubDirectoryName[] = "rebs";

// The name of the sub directory inside the current working directory when
// isolated to a local universe.
constexpr char kLocalTempSubdirectoryName[] = ".build";

std::filesystem::path temp_directory_path;

}  // namespace

void InitializeTempDirectory() {
  std::filesystem::path temp_directory_root;
  if (IsThereALocalConfig()) {
    temp_directory_root = kLocalTempSubdirectoryName;
  } else {
    temp_directory_root =
        std::filesystem::temp_directory_path() / kTempSubDirectoryName;
  }
  temp_directory_path =
      temp_directory_root / OptimizationLevelToString(GetOptimizationLevel());
  EnsureDirectoriesAndParentsExist(temp_directory_path);
  SetPlaceholder("temp directory", std::string(temp_directory_path));
}

std::filesystem::path GetTempDirectoryPath() { return temp_directory_path; }

std::filesystem::path GetTempDirectoryPathForPackageName(
    const std::string& package_name) {
  return GetTempDirectoryPathForPackageID(GetIDOfPackageFromName(package_name));
}

std::filesystem::path GetTempDirectoryPathForPackagePath(
    const std::filesystem::path& path) {
  return GetTempDirectoryPathForPackageID(GetIDOfPackageFromPath(path));
}

// Returns the temp directory of a package from the package id.
std::filesystem::path GetTempDirectoryPathForPackageID(size_t package_id) {
  std::filesystem::path path = temp_directory_path / std::to_string(package_id);
  EnsureDirectoriesAndParentsExist(path);
  return path;
}

void EnsureDirectoriesAndParentsExist(const std::filesystem::path& path) {
  try {
    std::filesystem::create_directories(path);
  } catch (const std::filesystem::filesystem_error& err) {
    std::cerr << "Cannot create directory " << std::quoted(path.c_str())
              << " because: " << err.what() << std::endl;
  }
}

void DeleteFolderIfItExists(const std::filesystem::path& path) {
  try {
    if (std::filesystem::exists(path)) {
      std::filesystem::remove_all(path);
    }
  } catch (const std::filesystem::filesystem_error& err) {
    std::cerr << "Cannot delete directory " << std::quoted(path.c_str())
              << " because: " << err.what() << std::endl;
  }
}
