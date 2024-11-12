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

#include "packages.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "config.h"
#include "invocation.h"
#include "string_replace.h"
#include "temp_directory.h"

namespace {

// The name of the sub directory inside the temporary directory where to place
// the dynamically linked shared libraries.
constexpr char kDynamicLibrariesSubdirectoryName[] = "dynamic_libraries";

// The name of the sub directory inside the temporary directory where ot place
// the statically linked shared libraries.
constexpr char kStaticLibrariesSubdirectoryName[] = "static_libraries";

// Empty string, so "" can be returned as a const&.
std::filesystem::path kEmptyPath = "";

// Map of packages to where the packages live.
std::map<std::string, std::filesystem::path> packages_to_paths;

// The path of the temporary directory where the dynamically built libraries
// live.
std::filesystem::path dynamic_library_directory_path;

// The path of the temporary directory where the statically buiilt libraries
// live.
std::filesystem::path static_library_directory_path;

// Returns if the provided package looks like a path.
bool IsPackageAPath(const std::string& name_or_path) {
  return name_or_path.size() > 0 &&
         (name_or_path[0] == '.' || name_or_path[0] == '/' ||
          name_or_path.find(':') != std::string::npos);
}

void RegisterPackagePath(const std::string& package_path) {
  std::string package_name = GetPackageNameFromPath(package_path);
  if (packages_to_paths.find(package_name) != packages_to_paths.end()) {
    // The package has already been defined early on. So this instance iwll
    // be ignored.
    return;
  }
  packages_to_paths[package_name] = package_path;
}

}  // namespace

void InitializePackages() {
  // Register the packages directly mentioned in the input first.
  if (!RunOnAllKnownPackages()) {
    ForEachRawInputPackage([](const std::string& name_or_path) {
      // The current directory or any absolute directory should be registered as
      // packages first.
      if (name_or_path.size() == 0) {
        RegisterPackagePath(std::filesystem::current_path());
      } else if (IsPackageAPath(name_or_path)) {
        if (std::filesystem::exists(name_or_path)) {
          RegisterPackagePath(name_or_path);
        }
      }
    });
  }

  ForEachPackageDirectory([](const std::filesystem::path& package_directory) {
    if (!std::filesystem::exists(package_directory)) return;
    for (auto const& dir_entry :
         std::filesystem::directory_iterator{package_directory}) {
      // Skip files.
      if (!dir_entry.is_directory()) continue;
      std::filesystem::path path = dir_entry.path();

      // Skip hidden directories.
      std::string filename = path.filename();
      if (filename.size() == 0 || filename[0] == '.') continue;

      RegisterPackagePath(path);
    }
  });

  dynamic_library_directory_path =
      GetTempDirectoryPath() / kDynamicLibrariesSubdirectoryName;
  EnsureDirectoriesAndParentsExist(dynamic_library_directory_path);
  SetPlaceholder("shared_library_path",
                 (std::stringstream()
                  << std::quoted(dynamic_library_directory_path.c_str()))
                     .str());

  static_library_directory_path =
      GetTempDirectoryPath() / kStaticLibrariesSubdirectoryName;
  EnsureDirectoriesAndParentsExist(static_library_directory_path);
}

std::filesystem::path GetPackagePath(const std::string& name_or_path) {
  if (name_or_path.size() == 0) {
    return std::filesystem::current_path();
  } else if (IsPackageAPath(name_or_path)) {
    if (!std::filesystem::exists(name_or_path)) {
      std::cerr << "This looks like a path: " << std::quoted(name_or_path)
                << " but it can't be found." << std::endl;
      return "";
    }
    return name_or_path;
  } else {
    auto itr = packages_to_paths.find(name_or_path);
    if (itr == packages_to_paths.end()) {
      std::cout << "Can't find package named: " << std::quoted(name_or_path)
                << "." << std::endl;
      return "";
    }
    return itr->second;
  }
}

const std::filesystem::path& GetPackagePathFromName(const std::string& name) {
  auto itr = packages_to_paths.find(name);
  if (itr == packages_to_paths.end()) {
    std::cout << "Can't find package named: " << std::quoted(name) << "."
              << std::endl;
    return kEmptyPath;
  }
  return itr->second;
}

std::string GetPackageNameFromPath(const std::filesystem::path& path) {
  return path.filename();
}

void ForEachKnownPackage(
    const std::function<void(const std::string&)>& on_each_package) {
  for (const auto& itr : packages_to_paths) {
    on_each_package(itr.second);
  }
}

void ForEachInputPackage(
    const std::function<void(const std::string&)>& on_each_package) {
  if (RunOnAllKnownPackages()) {
    ForEachKnownPackage(on_each_package);
  } else {
    ForEachRawInputPackage([on_each_package](const std::string& raw_package) {
      std::string package_path = GetPackagePath(raw_package);
      if (!package_path.empty()) on_each_package(package_path);
    });
  }
}

const std::filesystem::path& GetDynamicLibraryDirectoryPath() {
  return dynamic_library_directory_path;
}

const std::filesystem::path& GetStaticLibraryDirectoryPath() {
  return static_library_directory_path;
}
