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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "nlohmann/json_fwd.hpp"

// Loads in the config file or creates one if it doesn't exist. Returns whether
// it was successful.
bool LoadConfigFile();

// Returns the number of parallel tasks.
int GetNumberOfParallelTasks();

// Calls a function for each directory that may contain packages.
void ForEachPackageDirectory(
    const std::function<void(const std::filesystem::path&)>& on_each_directory);

// Returns whether there is a local config file, causing this build to be in an
// isolated universe.
bool IsThereALocalConfig();

// Returns the global run command if one is set, otherwise a blank string.
std::string_view GetGlobalRunCommand();

// Loads a config flie for a package. Populates the timestamp with when it
// was changed.
std::optional<::nlohmann::json> LoadConfigFileForPackage(
    const std::string& package_name, const std::filesystem::path& package_path,
    uint64_t& timestamp);

