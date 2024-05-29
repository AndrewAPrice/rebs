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

#include "dependencies.h"

#include <stddef.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "temp_directory.h"
#include "timestamps.h"

namespace {

// The name of the file that is created in a package's temporary directory that
// contains the object files and their dependencies.
constexpr char kDependenciesFile[] = "dependencies";

// The prefix of the name of the file that is temporarily used by compilers to
// write the dependencies to. There is one file per running thread.
constexpr char kThreadDependencyFilePrefix[] = "deps";

// A mapping of Package ID -> {File -> Dependencies}.
std::map<size_t,
         std::map<std::filesystem::path, std::vector<std::filesystem::path>>>
    dependencies_per_file_per_package;

// Set of package IDs whos dependencies have changed.
std::set<size_t> packages_with_invalidated_dependencies;

std::filesystem::path GetDependencyFilePathForPackage(size_t package_id) {
  return GetTempDirectoryPathForPackageID(package_id) / kDependenciesFile;
}

void MaybeLoadDependenciesForPackage(
    size_t package_id,
    std::map<std::filesystem::path, std::vector<std::filesystem::path>>&
        dependencies_per_file) {
  std::ifstream input_file(GetDependencyFilePathForPackage(package_id));
  if (!input_file.is_open()) return;
  std::vector<std::filesystem::path>* dependencies = nullptr;

  while (true) {
    std::string line;
    if (!std::getline(input_file, line)) break;

    if (line.size() == 0) {
      dependencies = nullptr;
      continue;
    }
    std::filesystem::path path = line;
    if (dependencies == nullptr) {
      std::filesystem::path path = line;
      auto [itr, _] = dependencies_per_file.insert(
          std::make_pair(path, std::vector<std::filesystem::path>()));
      dependencies = &itr->second;
    } else {
      dependencies->push_back(path);
    }
  }

  input_file.close();
}

std::map<std::filesystem::path, std::vector<std::filesystem::path>>*
GetDependenciesForPackage(size_t package_id) {
  auto itr = dependencies_per_file_per_package.find(package_id);
  if (itr != dependencies_per_file_per_package.end()) return &itr->second;
  auto [itr2, added] = dependencies_per_file_per_package.insert(std::make_pair(
      package_id,
      std::map<std::filesystem::path, std::vector<std::filesystem::path>>()));
  MaybeLoadDependenciesForPackage(package_id, itr2->second);
  return &itr2->second;
}

void WriteDependenciesForPackage(size_t package_id) {
  std::ofstream output_file(GetDependencyFilePathForPackage(package_id));
  if (!output_file.is_open()) {
    std::cerr << "Cannot write to "
              << GetDependencyFilePathForPackage(package_id)
              << ". Output cannot be cached." << std::endl;
    return;
  }

  auto* dependencies_per_file = GetDependenciesForPackage(package_id);

  for (const auto& file_and_dependencies : *dependencies_per_file) {
    output_file << file_and_dependencies.first.string() << std::endl;
    for (const auto& dependency : file_and_dependencies.second)
      output_file << dependency.string() << std::endl;
    output_file << std::endl;
  }

  output_file.close();
}

}  // namespace

bool AreDependenciesNewerThanFile(size_t package_id, size_t threshold_timestamp,
                                  std::filesystem::path file) {
  size_t timestamp_of_destination = GetTimestampOfFile(file);

  // File doesn't exist, so it needs to be created, or it's older than the
  // treshold.
  if (timestamp_of_destination == 0 ||
      threshold_timestamp > timestamp_of_destination) {
    return true;
  }

  auto* dependencies_per_file = GetDependenciesForPackage(package_id);

  auto itr = dependencies_per_file->find(file);

  // Don't know what the dependencies of this file are, so it needs to be
  // recalculated.
  if (itr == dependencies_per_file->end()) return true;

  for (const auto& dependency : itr->second) {
    auto timestamp_of_dependency = GetTimestampOfFile(dependency);

    if (timestamp_of_dependency == 0 ||
        timestamp_of_dependency > timestamp_of_destination) {
      // Either the dependency disappeared or is newer than the destination.
      return true;
    }
  }
  return false;
}

void SetDependenciesOfFile(
    size_t package_id, const std::filesystem::path& file,
    const std::vector<std::filesystem::path>& dependencies) {
  auto* dependencies_per_file = GetDependenciesForPackage(package_id);
  auto itr = dependencies_per_file->find(file);
  if (itr != dependencies_per_file->end()) {
    const auto& old_dependencies = itr->second;
    if (old_dependencies.size() == dependencies.size()) {
      bool anything_changed = false;
      for (int i = 0; i < dependencies.size() && !anything_changed; i++) {
        if (old_dependencies[i] != dependencies[i]) anything_changed = true;
      }
      if (!anything_changed) return;
    }
  }

  (*dependencies_per_file)[file] = dependencies;
  packages_with_invalidated_dependencies.insert(package_id);
}

void FlushDependencies() {
  for (size_t package_id : packages_with_invalidated_dependencies)
    WriteDependenciesForPackage(package_id);
}

std::vector<std::filesystem::path> ReadDependenciesFromFile(
    const std::string& dependency_file_path) {
  std::ifstream input_file(dependency_file_path);
  if (!input_file.is_open()) return {};
  std::stringstream buffer;
  buffer << input_file.rdbuf();
  input_file.close();

  std::string_view input_contents = buffer.view();

  bool encountered_first_colon = false;
  std::vector<std::filesystem::path> dependencies;
  int start_index = 0;
  int path_length = 0;

  auto is_escaped_space = [&input_contents](int index) -> bool {
    return input_contents[index] == '\\' &&
           (index + 1) < input_contents.size() &&
           input_contents[index + 1] == ' ';
  };

  auto maybe_add_path = [&start_index, &path_length, &dependencies,
                         &input_contents, &is_escaped_space]() {
    if (path_length <= 0) return;

    std::string path_str;
    path_str.reserve(path_length);
    bool encountered_non_space = false;
    for (int i = 0, index = start_index; i < path_length; i++, index++) {
      if (is_escaped_space(index)) {
        path_str += ' ';
        index++;  // Skip over escaped path.
      } else {
        path_str += input_contents[index];
        encountered_non_space = true;
      }
    }

    if (encountered_non_space) dependencies.push_back(path_str);
  };

  for (int i = 0; i < input_contents.size(); i++) {
    char c = input_contents[i];

    // Skip everything before and including the initial colon.
    if (!encountered_first_colon) {
      if (c == ':') {
        encountered_first_colon = true;
        start_index = i + 1;
      }
      continue;
    }

    // Handle spaces in the path.
    if (is_escaped_space(i)) {
      path_length++;
      i++;  // Skip over the following space.
      continue;
    }
    // Handle if it's a divider or new-line.
    if (c == ' ' || c == '\n' || c == '\r' || c == '\\') {
      maybe_add_path();
      start_index = i + 1;
      path_length = 0;
    } else {
      // Regular character that's part of a path.
      path_length++;
    }
  }

  maybe_add_path();

  return dependencies;
}

std::string GetTempDependencyFilePath(int thread_id) {
  return GetTempDirectoryPath() /
         (std::string(kThreadDependencyFilePrefix) + std::to_string(thread_id));
}
