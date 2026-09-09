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
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>

#include "clangd.h"
#include "command_queue.h"
#include "deferred_command.h"
#include "dependencies.h"
#include "execute.h"
#include "invocation.h"
#include "package_metadata.h"
#include "packages.h"
#include "stage.h"
#include "string_replace.h"
#include "temp_directory.h"
#include "third_party.h"
#include "timestamps.h"

namespace {

// Packages that have been added to the build queue at some point during this
// run.
std::set<std::string> compiled_packages;
std::set<std::string> compiled_test_packages;

// Returns whether the filename ends in "_test" (without extensions).
bool IsTestFile(const std::filesystem::path& path) {
  return path.stem().native().ends_with("_test");
}

// Returns whether the filename is "main" (without extensions).
bool IsMainFile(const std::filesystem::path& path) {
  return path.stem() == "main";
}

// The name of the subdirectory inside of the package's temporary directory to
// store the object files in.
constexpr char kObjectsSubDirectory[] = "objects";

template <typename T>
std::string JoinVectorWithPrefix(const std::vector<T>& elements,
                                 std::string_view prefix, bool quote = false) {
  std::stringstream ss;
  for (const auto& el : elements) {
    ss << " " << prefix;
    if constexpr (std::is_same_v<T, std::filesystem::path>) {
      if (quote) {
        ss << std::quoted(el.c_str());
      } else {
        ss << el.c_str();
      }
    } else {
      if (quote) {
        ss << std::quoted(el);
      } else {
        ss << el;
      }
    }
  }
  return ss.str();
}

// Builds the C includes arguments.
std::string BuildCIncludes(const PackageMetadata& metadata) {
  return JoinVectorWithPrefix(metadata.consolidated_includes, "-I", true);
}

// Builds the C pre-processor DEFINE arguments.
std::string BuildCDefines(const PackageMetadata& metadata) {
  std::string defines_str =
      JoinVectorWithPrefix(metadata.consolidated_defines, "-D", false);
  if (GetInvocationAction() == InvocationAction::Test) {
    defines_str += " -DTEST";
  }
  return defines_str;
}

// Converts a vector of paths to a space deliminted string of quoted paths.
std::string BuildStringOfFilesFromVectorOfFiles(
    const std::vector<std::filesystem::path>& paths) {
  return JoinVectorWithPrefix(paths, "", true);
}

std::string BuildStringOfStringsFromVectorOfStringAndPrefix(
    std::string_view prefix, const std::vector<std::string>& paths) {
  return JoinVectorWithPrefix(paths, prefix, true);
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

// Queues a deferred command with source and destination files.
void QueueDeferredCommand(Stage stage, size_t package_id,
                          std::string command_str,
                          const std::filesystem::path& destination_file = {},
                          const std::filesystem::path& source_file = {}) {
  auto command = std::make_unique<DeferredCommand>();
  command->command = std::move(command_str);
  command->destination_file = destination_file.string();
  command->source_file = source_file.string();
  command->package_id = package_id;
  QueueCommand(stage, std::move(command));
}

// Queues a command to copy a file and updates the destination timestamp.
void QueueCopyFile(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   size_t package_id = 0) {
  std::string cmd =
      (std::stringstream() << "cp " << std::quoted(source.c_str()) << " "
                           << std::quoted(destination.c_str()))
          .str();
  QueueDeferredCommand(Stage::CopyAssets, package_id, std::move(cmd),
                       destination, source);
  SetTimestampOfFileToNow(destination);
}

void CopyAssetIfNewer(const std::filesystem::path& source,
                      const std::filesystem::path& destination) {
  if (GetTimestampOfFile(source) <= GetTimestampOfFile(destination)) return;
  QueueCopyFile(source, destination);
}

void CopyAssetFilesForPackage(const PackageMetadata& metadata) {
  ForEachAssetFile(metadata, CopyAssetIfNewer);
}

void QueueArchiveCommand(const std::filesystem::path& output_path,
                         const std::vector<std::filesystem::path>& input_files,
                         const PackageMetadata& metadata) {
  std::string inputs = BuildStringOfFilesFromVectorOfFiles(input_files);

  std::string cmd_template = metadata.static_linker_command;
  if (cmd_template.empty()) {
    cmd_template = "ar rcs ${out} ${in}";
  }
  SetPlaceholder(
      "out", (std::stringstream() << std::quoted(output_path.c_str())).str());
  SetPlaceholder("in", inputs);
  ReplacePlaceholdersInString(cmd_template);

  QueueDeferredCommand(Stage::LinkLibrary, metadata.package_id,
                       std::move(cmd_template), output_path);
  SetTimestampOfFileToNow(output_path);
}

void QueueCompileCommand(const PackageMetadata& metadata,
                         const std::filesystem::path& source_file,
                         const std::filesystem::path& object_file,
                         std::string cmd_template) {
  SetPlaceholder(
      "out", (std::stringstream() << std::quoted(object_file.c_str())).str());
  SetPlaceholder(
      "in", (std::stringstream() << std::quoted(source_file.c_str())).str());
  ReplacePlaceholdersInString(cmd_template);
  QueueDeferredCommand(Stage::Compile, metadata.package_id,
                       std::move(cmd_template), object_file, source_file);
}

// Forward declaration.
bool BuildPackage(const std::string& package_name);

// Builds dependencies required by an application or active test target.
bool BuildDependencies(const PackageMetadata& metadata, bool is_testing) {
  if (!metadata.IsApplication() && !is_testing) return true;

  for (const auto& dependency : metadata.consolidated_dependencies) {
    if (!BuildPackage(dependency)) return false;
  }

  if (is_testing) {
    for (const auto& test_dependency : metadata.test_dependencies) {
      if (!BuildPackage(test_dependency)) return false;
    }
  }
  return true;
}

// Sets common placeholders for package build commands.
void SetPackagePlaceholders(const PackageMetadata& metadata,
                            const std::string& package_name) {
  SetPlaceholder("package name", package_name);
  SetPlaceholder("cdefines", BuildCDefines(metadata));
  SetPlaceholder("cincludes", BuildCIncludes(metadata));
}

void BuildAndLinkNormalPackage(const PackageMetadata& metadata,
                               const std::string& package_name) {
  size_t output_timestamp = GetTimestampOfFile(metadata.output_path);
  bool requires_linking = (output_timestamp == 0);
  size_t target_timestamp = output_timestamp;

  std::filesystem::path shared_library_path;
  if (metadata.IsLibrary()) {
    shared_library_path = GetDynamicLibraryDirectoryPath() /
                          (std::string("lib") + package_name + ".so");
    size_t shared_lib_timestamp = GetTimestampOfFile(shared_library_path);
    if (shared_lib_timestamp == 0) {
      requires_linking = true;
    }
    target_timestamp = std::min(target_timestamp, shared_lib_timestamp);

    if (!metadata.statically_linked_library_output_path.empty()) {
      size_t static_lib_timestamp =
          GetTimestampOfFile(metadata.statically_linked_library_output_path);
      if (static_lib_timestamp == 0) {
        requires_linking = true;
      }
      target_timestamp = std::min(target_timestamp, static_lib_timestamp);
    }
  }

  std::vector<std::filesystem::path> object_files_to_link;

  ForEachSourceFile(
      metadata, [&](const std::filesystem::path& source_file,
                    const std::filesystem::path& destination_file) {
        auto build_command_itr = metadata.build_commands_by_file_extension.find(
            source_file.extension());
        if (build_command_itr ==
            metadata.build_commands_by_file_extension.end())
          return;

        if (metadata.files_to_ignore.contains(source_file)) return;

        // Skip test files during normal package builds.
        if (IsTestFile(source_file)) return;

        auto object_file = destination_file.string() + ".o";
        object_files_to_link.push_back(object_file);

        if (!AreDependenciesNewerThanFile(metadata.package_id,
                                          metadata.metadata_timestamp,
                                          object_file)) {
          if (!requires_linking &&
              GetTimestampOfFile(object_file) > target_timestamp) {
            requires_linking = true;
          }
          return;
        }

        QueueCompileCommand(metadata, source_file, object_file,
                            build_command_itr->second);
        requires_linking = true;
      });

  for (const auto& library_object :
       metadata.statically_linked_library_objects) {
    object_files_to_link.push_back(library_object);
    if (!requires_linking) {
      size_t library_timestamp = GetTimestampOfFile(library_object);
      if (library_timestamp == 0 ||
          library_timestamp > metadata.metadata_timestamp ||
          library_timestamp > target_timestamp) {
        requires_linking = true;
      }
    }
  }

  if (!requires_linking) return;

  std::string input_files =
      BuildStringOfFilesFromVectorOfFiles(object_files_to_link);
  SetPlaceholder("in", input_files);

  if (metadata.IsApplication()) {
    SetTimestampOfFileToNow(metadata.output_path);
    std::string cmd = metadata.statically_link ? metadata.static_linker_command
                                               : metadata.linker_command;
    SetPlaceholder("out", (std::stringstream()
                           << std::quoted(metadata.output_path.c_str()))
                              .str());
    SetPlaceholder("shared_libraries",
                   BuildStringOfStringsFromVectorOfStringAndPrefix(
                       "-l ", metadata.dynamically_linked_libaries));
    SetPlaceholder(
        "library_search_paths",
        JoinVectorWithPrefix(metadata.consolidated_library_search_directories,
                             "-L", true));
    ReplacePlaceholdersInString(cmd);

    QueueDeferredCommand(GetLinkerStage(metadata), metadata.package_id,
                         std::move(cmd), metadata.output_path);
  } else if (metadata.IsLibrary()) {
    // Dynamically link.
    SetTimestampOfFileToNow(shared_library_path);
    std::string cmd = metadata.linker_command;
    SetPlaceholder(
        "out", (std::stringstream() << std::quoted(shared_library_path.c_str()))
                   .str());
    ReplacePlaceholdersInString(cmd);
    QueueDeferredCommand(GetLinkerStage(metadata), metadata.package_id,
                         std::move(cmd), shared_library_path);

    // Copy the file to the destination directory.
    QueueCopyFile(shared_library_path, metadata.output_path,
                  metadata.package_id);

    // Statically link.
    QueueArchiveCommand(metadata.statically_linked_library_output_path,
                        object_files_to_link, metadata);
  }
}

void BuildAndLinkTestPackage(const PackageMetadata& metadata,
                             const std::string& package_name) {
  auto test_lib_output_path =
      metadata.temp_directory / (package_name + "_lib.a");
  auto test_exec_output_path =
      metadata.temp_directory / (package_name + "_test");

  size_t test_lib_timestamp = GetTimestampOfFile(test_lib_output_path);
  size_t test_exec_timestamp = GetTimestampOfFile(test_exec_output_path);

  bool lib_requires_linking = (test_lib_timestamp == 0);
  bool test_exec_requires_linking =
      lib_requires_linking || (test_exec_timestamp == 0);

  std::vector<std::filesystem::path> object_files_to_link;
  std::vector<std::filesystem::path> test_object_files_to_link;

  ForEachSourceFile(
      metadata, [&](const std::filesystem::path& source_file,
                    const std::filesystem::path& destination_file) {
        auto build_command_itr = metadata.build_commands_by_file_extension.find(
            source_file.extension());
        if (build_command_itr ==
            metadata.build_commands_by_file_extension.end())
          return;

        if (metadata.files_to_ignore.contains(source_file)) return;

        bool is_test = IsTestFile(source_file);
        // Skip the main file if testing this package.
        if (!is_test && IsMainFile(source_file)) return;

        auto object_file = destination_file.string() + ".o";

        if (is_test) {
          test_object_files_to_link.push_back(object_file);
        } else {
          object_files_to_link.push_back(object_file);
        }

        if (!AreDependenciesNewerThanFile(metadata.package_id,
                                          metadata.metadata_timestamp,
                                          object_file)) {
          size_t obj_timestamp = GetTimestampOfFile(object_file);
          if (is_test) {
            if (!test_exec_requires_linking &&
                obj_timestamp > test_exec_timestamp) {
              test_exec_requires_linking = true;
            }
          } else {
            if (!lib_requires_linking && obj_timestamp > test_lib_timestamp) {
              lib_requires_linking = true;
              test_exec_requires_linking = true;
            }
          }
          return;
        }

        QueueCompileCommand(metadata, source_file, object_file,
                            build_command_itr->second);
        if (is_test) {
          test_exec_requires_linking = true;
        } else {
          lib_requires_linking = true;
          test_exec_requires_linking = true;
        }
      });

  for (const auto& library_object :
       metadata.statically_linked_library_objects) {
    if (!test_exec_requires_linking) {
      size_t library_timestamp = GetTimestampOfFile(library_object);
      if (library_timestamp == 0 ||
          library_timestamp > metadata.metadata_timestamp ||
          library_timestamp > test_exec_timestamp) {
        test_exec_requires_linking = true;
      }
    }
  }

  if (lib_requires_linking) {
    QueueArchiveCommand(test_lib_output_path, object_files_to_link, metadata);
  }

  if (test_exec_requires_linking) {
    std::vector<std::filesystem::path> files_to_link =
        test_object_files_to_link;
    files_to_link.push_back(test_lib_output_path);
    for (const auto& library_object :
         metadata.statically_linked_library_objects) {
      files_to_link.push_back(library_object);
    }
    std::string input_files =
        BuildStringOfFilesFromVectorOfFiles(files_to_link);
    SetPlaceholder("in", input_files);

    SetTimestampOfFileToNow(test_exec_output_path);

    std::string compiler = "clang++";
    auto cc_itr = metadata.build_commands_by_file_extension.find(".cc");
    if (cc_itr != metadata.build_commands_by_file_extension.end()) {
      std::string cmd = cc_itr->second;
      size_t first_space = cmd.find(' ');
      if (first_space != std::string::npos) {
        compiler = cmd.substr(0, first_space);
      }
    }

    std::stringstream cmd_stream;
    cmd_stream << compiler << " -o "
               << std::quoted(test_exec_output_path.c_str()) << " "
               << input_files;

    if (!metadata.dynamically_linked_libaries.empty()) {
      cmd_stream << BuildStringOfStringsFromVectorOfStringAndPrefix(
          " -l", metadata.dynamically_linked_libaries);
    }
    bool has_dynamic_lib_dir = false;
    for (const auto& dir : metadata.consolidated_library_search_directories) {
      if (dir == GetDynamicLibraryDirectoryPath()) {
        has_dynamic_lib_dir = true;
      }
      cmd_stream << " -L" << std::quoted(dir.c_str());
    }
    if (!has_dynamic_lib_dir) {
      cmd_stream << " -L"
                 << std::quoted(GetDynamicLibraryDirectoryPath().c_str());
    }

    QueueDeferredCommand(Stage::LinkApplication, metadata.package_id,
                         cmd_stream.str(), test_exec_output_path);
  }
}

void CopyPackageAssets(const PackageMetadata& metadata) {
  if (metadata.destination_directory.empty()) return;

  if (!metadata.asset_directories.empty()) {
    CopyAssetFilesForPackage(metadata);
  }
  // Copy vcpkg runtime assets (e.g. DLLs, .so)
  for (const auto& dir : metadata.consolidated_runtime_search_directories) {
    if (!std::filesystem::exists(dir)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_directory()) continue;
      std::string ext = entry.path().extension().string();
      if (ext == ".dll" || ext == ".so" || ext == ".dylib") {
        CopyAssetIfNewer(entry.path(), metadata.destination_directory /
                                           entry.path().filename());
      }
    }
  }
}

bool BuildPackage(const std::string& package_name) {
  bool is_testing_this_package =
      (GetInvocationAction() == InvocationAction::Test &&
       package_name == GetActiveTestTarget());

  auto& visited =
      is_testing_this_package ? compiled_test_packages : compiled_packages;
  if (!visited.insert(package_name).second) return true;

  std::filesystem::path package_path = GetPackagePathFromName(package_name);
  if (!MaybeUpdateThirdPartyBeforeBuilding(package_path)) return false;
  MaybeGenerateClangdForPackage(package_name);

  auto metadata = GetMetadataForPackage(package_name);
  if (metadata == nullptr) {
    std::cerr << "Unable to build " << std::quoted(package_name) << "."
              << std::endl;
    return false;
  }

  if (GetInvocationAction() == InvocationAction::Test &&
      metadata->skip_for_tests) {
    return true;
  }

  if (!BuildDependencies(*metadata, is_testing_this_package)) return false;

  if (!metadata->destination_directory.empty()) {
    EnsureDirectoriesAndParentsExist(metadata->destination_directory);
  }

  if (!metadata->no_output_file) {
    SetPackagePlaceholders(*metadata, package_name);
    if (is_testing_this_package) {
      BuildAndLinkTestPackage(*metadata, package_name);
    } else {
      BuildAndLinkNormalPackage(*metadata, package_name);
    }
  }

  CopyPackageAssets(*metadata);
  return true;
}

// Initialize placeholder strings.
void InitializePlaceholders() {
  // Prevents ${deps file} from being substituted because it's replaced right
  // before executing with a thread-specific file path.
  SetPlaceholder("deps file", "${deps file}");

  SetLazyPlaceholder("clangresources", []() -> std::optional<std::string> {
    std::stringstream output;
    if (ExecuteCommand("clang -print-resource-dir", &output)) {
      std::string result = output.str();
      // Trim trailing newline
      if (!result.empty() && result.back() == '\n') {
        result.pop_back();
      }
      return result;
    }
    return std::nullopt;
  });
}

}  // namespace

bool BuildPackages() {
  InitializePlaceholders();
  bool successful = true;
  ForEachInputPackage([&successful](const std::string& package_path) {
    std::string package_name = GetPackageNameFromPath(package_path);
    if (GetInvocationAction() == InvocationAction::Test) {
      SetActiveTestTarget(package_name);
    }
    successful &= BuildPackage(package_name);
  });

  if (GetInvocationAction() == InvocationAction::Test) {
    SetActiveTestTarget("");
  }
  return successful;
}
