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
#include <map>
#include <string>
#include <vector>

// The type of package this is.
enum class PackageType { Application, Library };

// All the metadata representing a package.
struct PackageMetadata {
  // The type of package this is.
  PackageType type;
  // Returns whether the package is an application.
  bool IsApplication() const { return type == PackageType::Application; }
  // Returns whether the package is a library.
  bool IsLibrary() const { return type == PackageType::Library; }
  // The unique ID of this package.
  size_t package_id;

  // A map of file extensions to build commands to build the source files in
  // this package..
  std::map<std::string, std::string> build_commands_by_file_extension;
  // The linker command to build this package.
  std::string linker_command;

  // The path of the package's root directory.
  std::filesystem::path package_path;
  // The path of the temporary directory for the intermediate build files for
  // this package.
  std::filesystem::path temp_directory;
  // The path to the final output object once this package is built.
  std::filesystem::path output_object;

  // A list of source directories. These directories are recursively scanned for
  // source files to build.
  std::vector<std::string> source_directories;
  // A list of directories to scan for 'include' files, that is also shared with
  // any package dependencing on this package.
  std::vector<std::string> public_include_directories;
  // // A list of directories to scan for 'include' files.
  std::vector<std::string> include_directories;

  // A list of `define`s passed to source files when building, that is also
  // shared with any package depending on this package.
  std::vector<std::string> public_defines;
  // A list of `define`s passed to source files when building.
  std::vector<std::string> defines;
  // A list of packages that this package depends on.
  std::vector<std::string> dependencies;
  // A list of files to ignore when building.
  std::vector<std::string> files_to_ignore;
  // The timestamp of when the metadata was last updated.
  uint64_t metadata_timestamp;

  // Whether this package should skip building.
  bool should_skip;

  // Whether this metadata has consolidated information. The consolidated
  // fields contain the data consolidated from all of the dependencies that is
  // needed to build this package.
  bool has_consolidated_information;

  // The consolidated 'define's passed to the source files when building.
  std::vector<std::string> consolidated_defines;
  // The consolidated dependencies this package depends on.
  std::vector<std::string> consolidated_dependencies;
  // The consolidated list of directoriees to scan for 'include' files.
  std::vector<std::filesystem::path> consolidated_includes;
  // The consolidated list of library objects to link. This is only set if this
  // package is an application.
  std::vector<std::filesystem::path> consolidated_library_objects;
};

// Returns the metadata for a package.
PackageMetadata* GetMetadataForPackage(const std::string& package_name);
