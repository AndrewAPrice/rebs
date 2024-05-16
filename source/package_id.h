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
#include <string>

// Packages are given unique IDs. They are unique based on the package's path,
// even if multiple packages happen to have the same name.

// Iniitalizes the package IDs from disk.
void InitializePackageIDs();

// Flushes any changes to the package IDs to disk.
void FlushPackageIDs();

// Returns a package ID from its name.
size_t GetIDOfPackageFromName(const std::string& package_name);

// Returns a package ID from a path.
size_t GetIDOfPackageFromPath(const std::filesystem::path& package_path);
