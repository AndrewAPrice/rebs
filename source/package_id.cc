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

#include "package_id.h"

#include <stddef.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "packages.h"
#include "temp_directory.h"
#include "timestamps.h"

namespace {

constexpr char kPackageIdFile[] = "package_ids";

size_t next_package_number = 0;
std::map<std::filesystem::path, size_t> package_path_to_id;
bool package_ids_invalidated = false;

std::filesystem::path GetPackageFilePath() {
  return GetTempDirectoryPath() / kPackageIdFile;
}

}  // namespace

void InitializePackageIDs() {
  std::ifstream input_file(GetPackageFilePath());
  if (input_file.is_open()) {
    size_t max_package_number = 0;
    std::string package_path;
    std::string package_id_str;
    while (true) {
      // Read the package path.
      if (!std::getline(input_file, package_path)) break;
      // Read the package ID.
      if (!std::getline(input_file, package_id_str)) break;

      char* last_char{};
      size_t package_id =
          std::strtoll(package_id_str.c_str(), &last_char, /*base=*/10);
      if (package_id_str.c_str() == last_char) continue;

      if (DoesFileExist(package_path)) {
        package_path_to_id[package_path] = package_id;
        max_package_number = std::max(max_package_number, package_id);
        EnsureDirectoriesAndParentsExist(
            GetTempDirectoryPathForPackageID(package_id));
      } else {
        DeleteFolderIfItExists(GetTempDirectoryPathForPackageID(package_id));
        package_ids_invalidated = true;
      }
    }
    input_file.close();
    next_package_number = max_package_number + 1;
  }
}

void FlushPackageIDs() {
  if (!package_ids_invalidated) return;

  std::ofstream output_file(GetPackageFilePath());
  if (!output_file.is_open()) {
    std::cerr << "Cannot write to " << GetPackageFilePath()
              << ". Output cannot be cached." << std::endl;
    return;
  }

  for (const auto& package_path_to_package_id : package_path_to_id) {
    output_file << package_path_to_package_id.first.string() << std::endl;
    output_file << std::to_string(package_path_to_package_id.second)
                << std::endl;
  }

  output_file.close();
}

size_t GetIDOfPackageFromName(const std::string& package_name) {
  return GetIDOfPackageFromPath(GetPackagePathFromName(package_name));
}

size_t GetIDOfPackageFromPath(const std::filesystem::path& package_path) {
  auto itr = package_path_to_id.find(package_path);
  if (itr != package_path_to_id.end()) return itr->second;

  size_t id = next_package_number;
  next_package_number++;
  package_path_to_id[package_path] = id;
  package_ids_invalidated = true;

  EnsureDirectoriesAndParentsExist(GetTempDirectoryPathForPackageID(id));

  return id;
}
