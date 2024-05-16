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

#pragma once

#include <stddef.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Returns if the dependencies are newer than a file, or if there are no records
// of the dependencies.
bool AreDependenciesNewerThanFile(size_t package_id, size_t threshold_timestamp,
                                  std::filesystem::path file);

// Sets the dependencies of a file.
void SetDependenciesOfFile(
    size_t package_id, const std::filesystem::path& file,
    const std::vector<std::filesystem::path>& dependencies);

// Flush any changes to the dependencies to disk.
void FlushDependencies();

// Returns a list of dependencies from a Clang/GCC compatible file.
std::vector<std::filesystem::path> ReadDependenciesFromFile(
    const std::string& dependency_file_path);

// Returns the path of the compiler's dependency file that's unique to a thread.
std::string GetTempDependencyFilePath(int thread_id);
