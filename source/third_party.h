// Copyright 2025 Google LLC
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

// Updates the third party dependencies for the given package.
// Returns true if successful.
bool UpdateThirdParty(const std::filesystem::path &package_path,
                      bool force = false);

// Checks if there are third party dependencies that need to be updated for a
// package. This includes the intial build even if --update was not specified.
// Returns false if something failed. Returns true if successful or if nothing
// needed to be updated.
bool MaybeUpdateThirdPartyBeforeBuilding(
    const std::filesystem::path &package_path);

// Updates all third party packages.
bool UpdateThirdPartyPackages();

// Cleans the third party dependencies for the given package.
// Returns true if successful.
bool CleanThirdParty(const std::filesystem::path &package_path);

// Cleans the cached repositories directory.
// Returns true if successful.
bool CleanRepositoriesDirectory();
