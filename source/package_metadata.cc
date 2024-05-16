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

#include "package_metadata.h"

#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>

#include "config.h"
#include "nlohmann/json.hpp"
#include "package_id.h"
#include "packages.h"
#include "temp_directory.h"

namespace {

std::map<std::string, std::unique_ptr<PackageMetadata>>
    metadata_by_package_name;

void PopulateVectorOfStringsFromConfigArray(nlohmann::json& config_array,
                                            std::vector<std::string>& strings) {
  if (config_array.is_array()) {
    for (auto& str : config_array) {
      if (str.is_string()) {
        strings.push_back(str.template get<std::string>());
      }
    }
  }
}

bool ParseConfigIntoMetadata(const std::string& package_name,
                             nlohmann::json& config,
                             PackageMetadata& metadata) {
  auto& package_type = config["package_type"];
  if (package_type.is_string()) {
    std::string package_type_str = package_type.template get<std::string>();
    if (package_type_str == "application") {
      metadata.type = PackageType::Application;
    } else if (package_type_str == "library") {
      metadata.type = PackageType::Library;
    } else {
      std::cerr << "Package " << std::quoted(package_name)
                << " has unknown package type " << std::quoted(package_type_str)
                << "." << std::endl;
      return false;
    }
  } else {
    metadata.type = PackageType::Application;
  }

  auto& build_commands = config["build_commands"];
  if (build_commands.is_object()) {
    for (auto& build_command : build_commands.items()) {
      if (build_command.value().is_string()) {
        metadata.build_commands_by_file_extension["." + build_command.key()] =
            build_command.value().template get<std::string>();
      }
    }
  }

  auto& linker_command = config["linker_command"];
  if (linker_command.is_string()) {
    metadata.linker_command = linker_command.template get<std::string>();
  }

  PopulateVectorOfStringsFromConfigArray(config["source_directories"],
                                         metadata.source_directories);
  PopulateVectorOfStringsFromConfigArray(config["public_include_directories"],
                                         metadata.public_include_directories);
  PopulateVectorOfStringsFromConfigArray(config["include_directories"],
                                         metadata.include_directories);

  /*  auto& include_priority = config["include_priority"];
    if (include_priority.is_number()) {
      metadata.include_priority = include_priority.template get<int>();
    }
    */

  PopulateVectorOfStringsFromConfigArray(config["public_defines"],
                                         metadata.public_defines);
  PopulateVectorOfStringsFromConfigArray(config["defines"], metadata.defines);
  PopulateVectorOfStringsFromConfigArray(config["dependencies"],
                                         metadata.dependencies);
  PopulateVectorOfStringsFromConfigArray(config["files_to_ignore"],
                                         metadata.files_to_ignore);

  auto& should_skip = config["should_skip"];
  if (should_skip.is_boolean()) {
  }

  return true;
}

PackageMetadata* GetUnconsolidatedMetadataForPackage(
    const std::string& package_name) {
  auto itr = metadata_by_package_name.find(package_name);
  if (itr != metadata_by_package_name.end()) return itr->second.get();

  std::filesystem::path package_path = GetPackagePathFromName(package_name);
  if (package_path == "") return nullptr;

  auto metadata = std::make_unique<PackageMetadata>();
  auto config = LoadConfigFileForPackage(package_name, package_path,
                                         metadata->metadata_timestamp);
  if (!config) return nullptr;
  metadata->package_path = package_path;
  metadata->temp_directory = GetTempDirectoryPathForPackagePath(package_path);
  metadata->output_object = metadata->temp_directory / package_name;
  metadata->package_id = GetIDOfPackageFromPath(package_path);
  auto& output_extension = (*config)["output_extension"];
  if (output_extension.is_string()) {
    std::string extension = output_extension.template get<std::string>();
    if (extension.length() > 0) metadata->output_object += "." + extension;
  }

  if (!ParseConfigIntoMetadata(package_name, *config, *metadata))
    return nullptr;
  PackageMetadata* metadata_ptr = metadata.get();
  metadata_by_package_name[package_name] = std::move(metadata);
  return metadata_ptr;
}

bool ConsolidateMetadataForPackage(const std::string& package_name,
                                   PackageMetadata& metadata) {
  // Walk through each encountered dependencies and add anything they expose to
  // this package.
  std::set<std::string> encountered_dependencies;
  encountered_dependencies.insert(package_name);

  std::queue<std::string> dependencies_to_visit;
  auto add_dependency = [&encountered_dependencies, &dependencies_to_visit](
                            const std::string& dependency) {
    if (encountered_dependencies.contains(dependency)) return;
    encountered_dependencies.insert(dependency);
    dependencies_to_visit.push(dependency);
  };

  auto add_include_directory = [&metadata](const std::filesystem::path& path) {
    if (std::filesystem::exists(path))
      metadata.consolidated_includes.push_back(path);
  };

  // Add initial dependencies from the top level package.
  for (const auto& dependency : metadata.dependencies) {
    add_dependency(dependency);
  }

  // Defines are stored in a set because there's a possibility there could be
  // duplicates.
  std::set<std::string> defines;

  // Add values from the top level package.
  for (const auto& define : metadata.defines) defines.insert(define);
  for (const auto& define : metadata.public_defines) defines.insert(define);
  for (const auto& include_directory : metadata.include_directories)
    add_include_directory(metadata.package_path / include_directory);
  for (const auto& include_directory : metadata.public_include_directories)
    add_include_directory(metadata.package_path / include_directory);

  // Walk through the dependencies.
  while (!dependencies_to_visit.empty()) {
    const auto& dependency = dependencies_to_visit.front();
    metadata.consolidated_dependencies.push_back(dependency);
    auto* child_metadata = GetUnconsolidatedMetadataForPackage(dependency);
    if (!child_metadata) {
      std::cerr << std::quoted(package_name) << " depends on "
                << std::quoted(dependency) << " but the latter isn't found."
                << std::endl;
      return false;
    }
    if (!child_metadata->IsLibrary()) {
      std::cerr << std::quoted(package_name) << " depends on "
                << std::quoted(dependency) << " but the latter isn't a library."
                << std::endl;
      return false;
    }

    // Add values from this package.

    if (metadata.IsApplication()) {
      metadata.consolidated_library_objects.push_back(
          child_metadata->output_object);
    }

    for (const auto& define : child_metadata->public_defines)
      defines.insert(define);

    for (const auto& include_directory :
         child_metadata->public_include_directories) {
      add_include_directory(child_metadata->package_path / include_directory);
    }

    metadata.metadata_timestamp = std::max(metadata.metadata_timestamp,
                                           child_metadata->metadata_timestamp);

    // Add sub-dependencies.
    for (const auto& subdependency : child_metadata->dependencies)
      add_dependency(subdependency);

    dependencies_to_visit.pop();
  }

  for (const auto& define : defines)
    metadata.consolidated_defines.push_back(define);

  metadata.has_consolidated_information = true;
  return true;
}

}  // namespace

PackageMetadata* GetMetadataForPackage(const std::string& package_name) {
  PackageMetadata* metadata = GetUnconsolidatedMetadataForPackage(package_name);
  if (metadata == nullptr) return nullptr;
  if (!metadata->has_consolidated_information) {
    if (!ConsolidateMetadataForPackage(package_name, *metadata)) return nullptr;
  }
  return metadata;
}
