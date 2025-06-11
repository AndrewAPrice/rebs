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

#include <filesystem>
#include <string>

// Initializes the temporary directory, creating it if it doesn't exist.
void InitializeTempDirectory();

// Finalizes the temp directory.
void FinalizeTempDirectory();

// Returns the temp path.
std::filesystem::path GetTempDirectoryPath();

// Ensures a directory exists. Creates it (and the parents) if neccesary.
void EnsureDirectoriesAndParentsExist(const std::filesystem::path& path);

// Deletes a folder if it exists, even if it contains data.
void DeleteFolderIfItExists(const std::filesystem::path& path);

// Returns the temp directory of a package from the package name.
std::filesystem::path GetTempDirectoryPathForPackageName(
    const std::string& package_name);

std::filesystem::path GetTempDirectoryPathForPackagePath(
    const std::filesystem::path& path);

// Returns the temp directory of a package from the package id.
std::filesystem::path GetTempDirectoryPathForPackageID(size_t package_id);

// Deletes the temporary directory for the current configuration.
void CleanCurrentConfigurationTempDirectory();
