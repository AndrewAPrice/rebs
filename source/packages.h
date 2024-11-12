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
#include <functional>
#include <string>

// Scans the system for packages and initializes it.
void InitializePackages();

// Gets the package path of a package from either it's name or a path.
std::filesystem::path GetPackagePath(const std::string& name_or_path);

// Returns the package's name from its path.
std::string GetPackageNameFromPath(const std::filesystem::path& path);

// Returns the package's path from its name.
const std::filesystem::path& GetPackagePathFromName(const std::string& name);

// Loops through each known package on the system and calls `on_each_package`
// with the package's path.
void ForEachKnownPackage(
    const std::function<void(const std::string&)>& on_each_package);

// Loops through each input package and calls `on_each_package` with the
// package's path.
void ForEachInputPackage(
    const std::function<void(const std::string&)>& on_each_package);

// Returns the path to where the built dynamically linked libraries live.
const std::filesystem::path& GetDynamicLibraryDirectoryPath();

// Returns the path to where the build statically linked libraries live.
const std::filesystem::path& GetStaticLibraryDirectoryPath();