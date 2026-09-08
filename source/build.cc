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

// Returns the starting and ending position of the stem of the filename (not
// including directory separators or extensions).
void GetStemBoundaries(const std::string& native_str, size_t& filename_start,
                       size_t& ext_start) {
  size_t last_slash = native_str.find_last_of("/\\");
  filename_start = (last_slash == std::string::npos) ? 0 : last_slash + 1;

  size_t last_dot = native_str.find_last_of('.');
  ext_start = (last_dot == std::string::npos || last_dot < filename_start)
                  ? native_str.size()
                  : last_dot;
}

// Returns whether the filename ends in "_test" (without extensions).
bool IsTestFile(const std::filesystem::path& path) {
  const auto& native_str = path.native();
  size_t filename_start = 0;
  size_t ext_start = 0;
  GetStemBoundaries(native_str, filename_start, ext_start);

  size_t stem_len = ext_start - filename_start;
  return stem_len >= 5 && native_str.compare(ext_start - 5, 5, "_test") == 0;
}

// Returns whether the filename is "main" (without extensions).
bool IsMainFile(const std::filesystem::path& path) {
  const auto& native_str = path.native();
  size_t filename_start = 0;
  size_t ext_start = 0;
  GetStemBoundaries(native_str, filename_start, ext_start);

  size_t stem_len = ext_start - filename_start;
  return stem_len == 4 && native_str.compare(filename_start, 4, "main") == 0;
}

// The name of the subdirectory inside of the package's temporary directory to
// store the object files in.
constexpr char kObjectsSubDirectory[] = "objects";

template <typename T>
std::string JoinVectorWithPrefix(const std::vector<T>& elements, std::string_view prefix, bool quote = false) {
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
  std::string defines_str = JoinVectorWithPrefix(metadata.consolidated_defines, "-D", false);
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

void QueueArchiveCommand(const std::filesystem::path& output_path,
                         const std::vector<std::filesystem::path>& input_files,
                         const PackageMetadata& metadata) {
  std::string inputs = BuildStringOfFilesFromVectorOfFiles(input_files);

  auto command = std::make_unique<DeferredCommand>();
  std::string cmd_template = metadata.static_linker_command;
  if (cmd_template.empty()) {
    cmd_template = "ar rcs ${out} ${in}";
  }
  command->command = cmd_template;
  SetPlaceholder("out", (std::stringstream() << std::quoted(output_path.c_str())).str());
  SetPlaceholder("in", inputs);
  ReplacePlaceholdersInString(command->command);

  command->destination_file = output_path;
  command->package_id = metadata.package_id;

  QueueCommand(Stage::LinkLibrary, std::move(command));
}

bool BuildPackage(const std::string& package_name) {
  bool is_testing_this_package =
      (GetInvocationAction() == InvocationAction::Test &&
       package_name == GetActiveTestTarget());

  if (is_testing_this_package) {
    if (compiled_test_packages.contains(package_name)) return true;
    compiled_test_packages.insert(package_name);
  } else {
    if (compiled_packages.contains(package_name)) return true;
    compiled_packages.insert(package_name);
  }

  std::filesystem::path package_path = GetPackagePathFromName(package_name);
  if (!MaybeUpdateThirdPartyBeforeBuilding(package_path))
    return false;
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

  // Applications should build dependent libraries first.
  if (metadata->IsApplication() || is_testing_this_package) {
    for (const auto& dependency : metadata->consolidated_dependencies)
      if (!BuildPackage(dependency)) return false;

    if (is_testing_this_package) {
      for (const auto& test_dependency : metadata->test_dependencies) {
        if (!BuildPackage(test_dependency)) return false;
      }
    }
  }

  if (!metadata->destination_directory.empty())
    EnsureDirectoriesAndParentsExist(metadata->destination_directory);

  if (!metadata->no_output_file) {
    std::vector<std::filesystem::path> object_files_to_link;

    SetPlaceholder("package name", std::string(package_name));
    SetPlaceholder("cdefines", BuildCDefines(*metadata));
    SetPlaceholder("cincludes", BuildCIncludes(*metadata));

    bool requires_linking = false;

    std::vector<std::filesystem::path> test_object_files_to_link;

    ForEachSourceFile(
        *metadata, [metadata, &object_files_to_link, &test_object_files_to_link,
                    &requires_linking, is_testing_this_package](
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

          bool is_test = IsTestFile(source_file);
          // Ignore test files if not running tests.
          if (GetInvocationAction() != InvocationAction::Test && is_test)
            return;

          // If compiling the test executable for this package, split the files
          // between tests and non-test files.
          if (is_testing_this_package) {
            if (!is_test && IsMainFile(source_file))
              return;  // Skip the main file if testing this package.
          } else {
            // If compiling a regular dependency during a test run, or a normal
            // build, ignore test files.
            if (is_test) return;
          }

          auto object_file = std::string(destination_file) + ".o";

          if (is_test) {
            test_object_files_to_link.push_back(object_file);
          } else {
            object_files_to_link.push_back(object_file);
          }

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

    if (is_testing_this_package) {
      std::filesystem::path test_lib_output_path =
          metadata->temp_directory / (package_name + "_lib.a");
      std::filesystem::path test_exec_output_path =
          metadata->temp_directory / (package_name + "_test");

      bool lib_requires_linking =
          requires_linking || !DoesFileExist(test_lib_output_path);
      bool test_exec_requires_linking =
          lib_requires_linking || !DoesFileExist(test_exec_output_path);

      size_t test_exec_timestamp = 0;
      if (DoesFileExist(test_exec_output_path)) {
        test_exec_timestamp = GetTimestampOfFile(test_exec_output_path);
      }

      if (!lib_requires_linking) {
        size_t test_lib_timestamp = GetTimestampOfFile(test_lib_output_path);
        for (const auto& object_file : object_files_to_link) {
          size_t obj_timestamp = GetTimestampOfFile(object_file);
          if (obj_timestamp == 0 || obj_timestamp > test_lib_timestamp) {
            lib_requires_linking = true;
            test_exec_requires_linking = true;
            break;
          }
        }
      }

      if (!test_exec_requires_linking) {
        for (const auto& test_obj : test_object_files_to_link) {
          size_t obj_timestamp = GetTimestampOfFile(test_obj);
          if (obj_timestamp == 0 || obj_timestamp > test_exec_timestamp) {
            test_exec_requires_linking = true;
            break;
          }
        }
      }

      for (const auto& library_object :
           metadata->statically_linked_library_objects) {
        if (!test_exec_requires_linking) {
          size_t library_timestamp = GetTimestampOfFile(library_object);
          if (library_timestamp == 0 ||
              library_timestamp > metadata->metadata_timestamp ||
              library_timestamp > test_exec_timestamp) {
            test_exec_requires_linking = true;
          }
        }
      }

      if (lib_requires_linking) {
        QueueArchiveCommand(test_lib_output_path, object_files_to_link, *metadata);
      }

      if (test_exec_requires_linking) {
        std::vector<std::filesystem::path> files_to_link =
            test_object_files_to_link;
        files_to_link.push_back(test_lib_output_path);
        for (const auto& library_object :
             metadata->statically_linked_library_objects) {
          files_to_link.push_back(library_object);
        }
        std::string input_files =
            BuildStringOfFilesFromVectorOfFiles(files_to_link);
        SetPlaceholder("in", input_files);

        SetTimestampOfFileToNow(test_exec_output_path);

        std::string compiler = "clang++";
        auto cc_itr = metadata->build_commands_by_file_extension.find(".cc");
        if (cc_itr != metadata->build_commands_by_file_extension.end()) {
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

        if (!metadata->dynamically_linked_libaries.empty()) {
          cmd_stream << BuildStringOfStringsFromVectorOfStringAndPrefix(
              " -l", metadata->dynamically_linked_libaries);
        }
        bool has_dynamic_lib_dir = false;
        for (const auto& dir :
             metadata->consolidated_library_search_directories) {
          if (dir == GetDynamicLibraryDirectoryPath()) {
            has_dynamic_lib_dir = true;
          }
          cmd_stream << " -L" << std::quoted(dir.c_str());
        }
        if (!has_dynamic_lib_dir) {
          cmd_stream << " -L"
                     << std::quoted(GetDynamicLibraryDirectoryPath().c_str());
        }

        auto command = std::make_unique<DeferredCommand>();
        command->command = cmd_stream.str();
        command->destination_file = test_exec_output_path;
        command->package_id = metadata->package_id;
        QueueCommand(Stage::LinkApplication, std::move(command));
      }

    } else {
      size_t object_file_timestamp = 0;
      if (DoesFileExist(metadata->output_path) && !requires_linking) {
        object_file_timestamp = GetTimestampOfFile(metadata->output_path);
      } else {
        requires_linking = true;
      }

      // Check if any of this package's object files are newer than the linked output.
      if (!requires_linking) {
        for (const auto& object_file : object_files_to_link) {
          size_t obj_timestamp = GetTimestampOfFile(object_file);
          if (obj_timestamp == 0 || obj_timestamp > object_file_timestamp) {
            requires_linking = true;
            break;
          }
        }
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
      if (!requires_linking && !shared_library_path.empty()) {
        if (!DoesFileExist(shared_library_path)) {
          requires_linking = true;
        } else {
          size_t shared_lib_timestamp = GetTimestampOfFile(shared_library_path);
          for (const auto& object_file : object_files_to_link) {
            size_t obj_timestamp = GetTimestampOfFile(object_file);
            if (obj_timestamp == 0 || obj_timestamp > shared_lib_timestamp) {
              requires_linking = true;
              break;
            }
          }
        }
      }

      if (!requires_linking && metadata->IsLibrary() &&
          !metadata->statically_linked_library_output_path.empty()) {
        if (!DoesFileExist(metadata->statically_linked_library_output_path)) {
          requires_linking = true;
        } else {
          size_t static_lib_timestamp =
              GetTimestampOfFile(metadata->statically_linked_library_output_path);
          for (const auto& object_file : object_files_to_link) {
            size_t obj_timestamp = GetTimestampOfFile(object_file);
            if (obj_timestamp == 0 || obj_timestamp > static_lib_timestamp) {
              requires_linking = true;
              break;
            }
          }
        }
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
          if (!metadata->consolidated_library_search_directories.empty()) {
            std::stringstream ss;
            for (const auto& dir :
                 metadata->consolidated_library_search_directories) {
              ss << " -L" << std::quoted(dir.c_str());
            }
            SetPlaceholder("library_search_paths", ss.str());
          } else {
            SetPlaceholder("library_search_paths", "");
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
          SetTimestampOfFileToNow(metadata->output_path);
          command = std::make_unique<DeferredCommand>();
          command->command =
              (std::stringstream()
               << "cp " << std::quoted(shared_library_path.c_str()) << " "
               << std::quoted(metadata->output_path.c_str()))
                  .str();
          command->destination_file = metadata->output_path;
          command->package_id = metadata->package_id;

          QueueCommand(Stage::CopyAssets, std::move(command));

          // Statically link.
          SetTimestampOfFileToNow(
              metadata->statically_linked_library_output_path);
          QueueArchiveCommand(metadata->statically_linked_library_output_path, object_files_to_link, *metadata);
        }
      }
    }
  }

  // Copy assets to the destination directory.
  if (!metadata->destination_directory.empty()) {
    if (!metadata->asset_directories.empty()) {
      CopyAssetFilesForPackage(*metadata);
    }
    // Copy vcpkg runtime assets (e.g. DLLs, .so)
    for (const auto& dir : metadata->consolidated_runtime_search_directories) {
      if (!std::filesystem::exists(dir)) continue;
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_directory()) continue;
        std::string ext = entry.path().extension().string();
        if (ext == ".dll" || ext == ".so" || ext == ".dylib") {
          CopyAssetIfNewer(entry.path(), metadata->destination_directory /
                                             entry.path().filename());
        }
      }
    }
  }

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
