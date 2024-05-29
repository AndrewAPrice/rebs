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

#include "build.h"

#include <filesystem>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>
#include <string>

#include "command_queue.h"
#include "deferred_command.h"
#include "dependencies.h"
#include "invocation.h"
#include "package_metadata.h"
#include "packages.h"
#include "stage.h"
#include "string_replace.h"
#include "temp_directory.h"
#include "timestamps.h"

namespace {

// Packages that have been added to the build queue at some point during this
// run.
std::set<std::string> packages;

// The name of the subdirectory inside of the package's temporary directory to
// store the object files in.
constexpr char kObjectsSubDirectory[] = "objects";

// Builds the C includes arguments.
std::string BuildCIncludes(const PackageMetadata& metadata) {
  std::stringstream c_includes;
  for (const auto& include : metadata.consolidated_includes)
    c_includes << " -I" << std::quoted(include.c_str());
  return c_includes.str();
}

// Builds the C pre-processor DEFINE arguments.
std::string BuildCDefines(const PackageMetadata& metadata) {
  std::stringstream c_defines;
  for (const auto& define : metadata.consolidated_defines)
    c_defines << " -D" << define;
  return c_defines.str();
}

// Converts a vector of paths to a space deliminted string of quoted paths.
std::string BuildStringOfFilesFromVectorOfFiles(
    const std::vector<std::filesystem::path>& paths) {
  std::stringstream str;
  for (const auto& path : paths) str << " " << std::quoted(path.c_str());
  return str.str();
}

// Returns the linker stage to use for a given package based on its metadata.
Stage GetLinkerStage(const PackageMetadata& metadata) {
  if (metadata.IsApplication()) return Stage::LinkApplication;
  return Stage::LinkLibrary;
}

// Recursively loops over a directory of source files.
void ForEachSourceFile(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& output_directory,
    const std::function<void(const std::filesystem::path&,
                             const std::filesystem::path&)>& on_each_file) {
  EnsureDirectoriesAndParentsExist(output_directory);
  for (auto const& dir_entry :
       std::filesystem::directory_iterator{source_directory}) {
    auto path = dir_entry.path();
    std::string filename = path.filename();
    // Skip hidden files.
    if (filename.size() == 0 || filename[0] == '.') continue;

    if (dir_entry.is_directory()) {
      ForEachSourceFile(path, output_directory / filename, on_each_file);
    } else {
      on_each_file(path, output_directory / (filename + ".o"));
    }
  }
}

// Loops over each source file in a package.
void ForEachSourceFile(
    const PackageMetadata& metadata,
    const std::function<void(const std::filesystem::path&,
                             const std::filesystem::path&)>& on_each_file) {
  std::filesystem::path objects_directory =
      metadata.temp_directory / kObjectsSubDirectory;
  for (const auto& source_directory : metadata.source_directories) {
    ForEachSourceFile(metadata.package_path / source_directory,
                      objects_directory / source_directory, on_each_file);
  }
}

// Builds a package, and returns if it was successful.
bool BuildPackage(const std::string& package_name) {
  // Skip over already built packages.
  if (packages.contains(package_name)) return true;
  packages.insert(package_name);

  auto metadata = GetMetadataForPackage(package_name);
  if (metadata == nullptr) {
    std::cerr << "Unable to build " << std::quoted(package_name) << "."
              << std::endl;
    return false;
  }

  // Applications should build dependent libraries first.
  if (metadata->IsApplication()) {
    for (const auto& dependency : metadata->consolidated_dependencies)
      if (!BuildPackage(dependency)) return false;
  }

  std::string c_includes = BuildCIncludes(*metadata);
  std::string c_defines = BuildCDefines(*metadata);
  std::vector<std::filesystem::path> object_files_to_link;

  bool has_something_been_built = false;

  ForEachSourceFile(*metadata, [metadata, &c_includes, &c_defines,
                                &object_files_to_link,
                                &has_something_been_built](
                                   const std::filesystem::path& source_file,
                                   const std::filesystem::path& object_file) {
    auto build_command_itr = metadata->build_commands_by_file_extension.find(
        source_file.extension());
    if (build_command_itr == metadata->build_commands_by_file_extension.end())
      return;

    object_files_to_link.push_back(object_file);

    if (!AreDependenciesNewerThanFile(
            metadata->package_id, metadata->metadata_timestamp, object_file)) {
      return;
    }

    // if (source_file.extension)
    auto command = std::make_unique<DeferredCommand>();
    command->command = build_command_itr->second;
    ReplaceSubstringInString(command->command, "${cdefines}", c_includes);
    ReplaceSubstringInString(command->command, "${cincludes}", c_defines);
    ReplaceSubstringInString(
        command->command, "${out}",
        (std::stringstream() << std::quoted(object_file.c_str())).str());
    ReplaceSubstringInString(
        command->command, "${in}",
        (std::stringstream() << std::quoted(source_file.c_str())).str());
    command->source_file = source_file;
    command->destination_file = object_file;
    command->package_id = metadata->package_id;
    QueueCommand(Stage::Compile, std::move(command));
    has_something_been_built = true;
  });

  size_t object_file_timestamp = 0;
  if (DoesFileExist(metadata->output_object) && !has_something_been_built) {
    object_file_timestamp = GetTimestampOfFile(metadata->output_object);
  } else {
    has_something_been_built = true;
  }

  for (const auto& library_object : metadata->consolidated_library_objects) {
    object_files_to_link.push_back(library_object);
    if (!has_something_been_built) {
      size_t library_timestamp = GetTimestampOfFile(library_object);
      if (library_timestamp == 0 ||
          library_timestamp > metadata->metadata_timestamp ||
          library_timestamp > object_file_timestamp) {
        has_something_been_built = true;
      }
    }
  }

  if (has_something_been_built) {
    SetTimestampOfFileToNow(metadata->output_object);
    auto command = std::make_unique<DeferredCommand>();
    command->command = metadata->linker_command;
    ReplaceSubstringInString(
        command->command, "${out}",
        (std::stringstream() << std::quoted(metadata->output_object.c_str()))
            .str());
    ReplaceSubstringInString(
        command->command, "${in}",
        BuildStringOfFilesFromVectorOfFiles(object_files_to_link));
    command->destination_file = metadata->output_object;
    command->package_id = metadata->package_id;

    QueueCommand(GetLinkerStage(*metadata), std::move(command));
  }

  return true;
}

}  // namespace

bool BuildPackages() {
  bool successful = true;
  ForEachInputPackage([&successful](const std::string& package_path) {
    successful &= BuildPackage(GetPackageNameFromPath(package_path));
  });
  return successful;
}
