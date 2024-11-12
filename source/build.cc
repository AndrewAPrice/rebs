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

std::string BuildStringOfStringsFromVectorOfStringAndPrefix(
    std::string_view prefix, const std::vector<std::string>& paths) {
  std::stringstream str;
  for (const auto& path : paths)
    str << " " << prefix << std::quoted(path.c_str());
  return str.str();
}

// Returns the linker stage to use for a given package based on its metadata.
Stage GetLinkerStage(const PackageMetadata& metadata) {
  if (metadata.IsApplication()) return Stage::LinkApplication;
  return Stage::LinkLibrary;
}

// Recursively loops over a directory of source files.
void ForEachFile(
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
      ForEachFile(path, output_directory / filename, on_each_file);
    } else {
      on_each_file(path, output_directory / filename);
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
    ForEachFile(metadata.package_path / source_directory,
                objects_directory / source_directory, on_each_file);
  }
}

void ForEachAssetFile(
    const PackageMetadata& metadata,
    const std::function<void(const std::filesystem::path&,
                             const std::filesystem::path&)>& on_each_file) {
  for (const auto& asset_directory : metadata.asset_directories) {
    ForEachFile(metadata.package_path / asset_directory,
                metadata.destination_directory, on_each_file);
  }
}

void CopyAssetIfNewer(const std::filesystem::path& source,
                      const std::filesystem::path& destination) {
  if (GetTimestampOfFile(source) <= GetTimestampOfFile(destination)) return;

  auto command = std::make_unique<DeferredCommand>();
  command->command =
      (std::stringstream() << "cp " << std::quoted(source.c_str()) << " "
                           << std::quoted(destination.c_str()))
          .str();
  QueueCommand(Stage::CopyAssets, std::move(command));

  SetTimestampOfFileToNow(destination);
}

void CopyAssetFilesForPackage(PackageMetadata& metadata) {
  ForEachAssetFile(metadata, CopyAssetIfNewer);
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

  if (!metadata->destination_directory.empty())
    EnsureDirectoriesAndParentsExist(metadata->destination_directory);

  if (!metadata->no_output_file) {
    std::vector<std::filesystem::path> object_files_to_link;

    SetPlaceholder("package name", std::string(package_name));
    SetPlaceholder("cdefines", BuildCDefines(*metadata));
    SetPlaceholder("cincludes", BuildCIncludes(*metadata));

    bool requires_linking = false;

    ForEachSourceFile(
        *metadata, [metadata, &object_files_to_link, &requires_linking](
                       const std::filesystem::path& source_file,
                       const std::filesystem::path& destination_file) {
          auto build_command_itr =
              metadata->build_commands_by_file_extension.find(
                  source_file.extension());
          if (build_command_itr ==
              metadata->build_commands_by_file_extension.end())
            return;

          if (metadata->files_to_ignore.find(source_file) !=
              metadata->files_to_ignore.end()) {
            return;
          }

          auto object_file = std::string(destination_file) + ".o";

          object_files_to_link.push_back(object_file);

          if (!AreDependenciesNewerThanFile(metadata->package_id,
                                            metadata->metadata_timestamp,
                                            object_file)) {
            return;
          }

          // if (source_file.extension)
          auto command = std::make_unique<DeferredCommand>();
          command->command = build_command_itr->second;
          SetPlaceholder(
              "out",
              (std::stringstream() << std::quoted(object_file.c_str())).str());
          SetPlaceholder(
              "in",
              (std::stringstream() << std::quoted(source_file.c_str())).str());
          ReplacePlaceholdersInString(command->command);
          command->source_file = source_file;
          command->destination_file = object_file;
          command->package_id = metadata->package_id;
          QueueCommand(Stage::Compile, std::move(command));
          requires_linking = true;
        });

    size_t object_file_timestamp = 0;
    if (DoesFileExist(metadata->output_path) && !requires_linking) {
      object_file_timestamp = GetTimestampOfFile(metadata->output_path);
    } else {
      requires_linking = true;
    }

    for (const auto& library_object :
         metadata->statically_linked_library_objects) {
      object_files_to_link.push_back(library_object);
      if (!requires_linking) {
        size_t library_timestamp = GetTimestampOfFile(library_object);
        if (library_timestamp == 0 ||
            library_timestamp > metadata->metadata_timestamp ||
            library_timestamp > object_file_timestamp) {
          requires_linking = true;
        }
      }
    }

    std::filesystem::path shared_library_path;
    if (metadata->IsLibrary())
      shared_library_path = GetDynamicLibraryDirectoryPath() /
                            (std::string("lib") + package_name + ".so");
    if (!requires_linking && !shared_library_path.empty() &&
        !DoesFileExist(shared_library_path)) {
      // The  shared variant does not exists and needs to be created.
      requires_linking = true;
    }

    if (requires_linking) {
      std::string input_files =
          BuildStringOfFilesFromVectorOfFiles(object_files_to_link);
      SetPlaceholder("in", input_files);

      if (metadata->IsApplication()) {
        SetTimestampOfFileToNow(metadata->output_path);
        auto command = std::make_unique<DeferredCommand>();
        command->command = metadata->statically_link
                               ? metadata->static_linker_command
                               : metadata->linker_command;
        SetPlaceholder("out", (std::stringstream()
                               << std::quoted(metadata->output_path.c_str()))
                                  .str());
        if (!metadata->dynamically_linked_libaries.empty()) {
          SetPlaceholder("shared_libraries",
                         BuildStringOfStringsFromVectorOfStringAndPrefix(
                             "-l ", metadata->dynamically_linked_libaries));
        }
        ReplacePlaceholdersInString(command->command);
        command->destination_file = metadata->output_path;
        command->package_id = metadata->package_id;

        QueueCommand(GetLinkerStage(*metadata), std::move(command));
      } else if (metadata->IsLibrary()) {
        // Dynamically link.
        SetTimestampOfFileToNow(shared_library_path);
        auto command = std::make_unique<DeferredCommand>();
        command->command = metadata->linker_command;
        SetPlaceholder("out", (std::stringstream()
                               << std::quoted(shared_library_path.c_str()))
                                  .str());
        ReplacePlaceholdersInString(command->command);
        command->destination_file = shared_library_path;
        command->package_id = metadata->package_id;
        QueueCommand(GetLinkerStage(*metadata), std::move(command));

        // Copy the file to the destination directory.
        SetTimestampOfFileToNow(metadata->output_filename);
        command = std::make_unique<DeferredCommand>();
        command->command =
            (std::stringstream() << "cp " << std::quoted(shared_library_path.c_str()) << " "
                                 << std::quoted(metadata->output_filename.c_str()))
                .str();
        command->destination_file = metadata->output_filename;
        command->package_id = metadata->package_id;

        QueueCommand(Stage::CopyAssets, std::move(command));

        // Statically link.
        SetTimestampOfFileToNow(metadata->statically_linked_library_output_path);
        command = std::make_unique<DeferredCommand>();
        command->command = metadata->static_linker_command;
        SetPlaceholder("out", (std::stringstream()
                               << std::quoted(metadata->statically_linked_library_output_path.c_str()))
                                  .str());
        ReplacePlaceholdersInString(command->command);
        command->destination_file = metadata->statically_linked_library_output_path;
        command->package_id = metadata->package_id;

        QueueCommand(GetLinkerStage(*metadata), std::move(command));
      }
    }
  }

  // Copy assets to the destination directory.
  if (!metadata->destination_directory.empty() &&
      !metadata->asset_directories.empty())
    CopyAssetFilesForPackage(*metadata);

  return true;
}

// Initialize placeholder strings.
void InitializePlaceholders() {
  // Prevents ${deps file} from being substituted because it's replaced right
  // before executing with a thread-specific file path.
  SetPlaceholder("deps file", "${deps file}");
}

}  // namespace

bool BuildPackages() {
  InitializePlaceholders();
  bool successful = true;
  ForEachInputPackage([&successful](const std::string& package_path) {
    successful &= BuildPackage(GetPackageNameFromPath(package_path));
  });
  return successful;
}
