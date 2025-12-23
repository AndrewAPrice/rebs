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

#include "clangd.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "package_metadata.h"
#include "packages.h"
#include "timestamps.h"

namespace {

// Returns the build command for the first matching extension found.
std::string GetBuildCommand(const PackageMetadata &metadata,
                            const std::vector<std::string> &extensions) {
  for (const auto &extension : extensions) {
    auto it = metadata.build_commands_by_file_extension.find(extension);
    if (it != metadata.build_commands_by_file_extension.end()) {
      return it->second;
    }
  }
  return "";
}

// Extracts flags from a build command string.
std::vector<std::string> ExtractFlags(const std::string &command) {
  std::vector<std::string> flags;
  if (command.empty())
    return flags;

  std::stringstream ss(command);
  std::string segment;
  bool first = true;
  while (std::getline(ss, segment, ' ')) {
    if (segment.empty())
      continue;
    // Skip the compiler executable (first element)
    if (first) {
      first = false;
      continue;
    }

    // Skip placeholders
    if (segment.find("${") != std::string::npos)
      continue;

    // Skip broken tokens from placeholders (e.g. "file}" from "${deps file}")
    if (segment.find("}") != std::string::npos)
      continue;

    // Compiler flags should start with - (e.g. -std=c++20, -I..., -D...)
    if (segment[0] != '-')
      continue;

    flags.push_back(segment);
  }
  return flags;
}

} // namespace

void MaybeGenerateClangdForPackage(const std::string &package_name) {
  PackageMetadata *metadata = GetMetadataForPackage(package_name);
  if (!metadata)
    return;

  std::filesystem::path clangd_path = metadata->package_path / ".clangd";

  // Check if .clangd is up to date.
  if (std::filesystem::exists(clangd_path)) {
    size_t clangd_timestamp = GetTimestampOfFile(clangd_path);
    if (clangd_timestamp >= metadata->metadata_timestamp) {
      return;
    }
  }

  std::string cpp_command = GetBuildCommand(*metadata, {".cc", ".cpp", ".cxx"});
  std::string c_command = GetBuildCommand(*metadata, {".c"});

  // Fallback: if no C++ command, use C command as default, and vice versa.
  std::string default_command = cpp_command;
  if (default_command.empty()) {
    default_command = c_command;
  }
  // If still empty, try any (though unlikely to be useful)
  if (default_command.empty() &&
      !metadata->build_commands_by_file_extension.empty()) {
    default_command =
        metadata->build_commands_by_file_extension.begin()->second;
  }

  std::ofstream clangd_file(clangd_path);
  if (!clangd_file.is_open()) {
    std::cerr << "Failed to open " << clangd_path << " for writing."
              << std::endl;
    return;
  }

  // Helper function to write flags to the .clangd file.
  auto write_flags =
      [&](const std::vector<std::string> &flags,
          const std::vector<std::filesystem::path> &includes = {},
          const std::vector<std::string> &defines = {}) {
        clangd_file << "CompileFlags:" << std::endl;
        clangd_file << "  Add: [" << std::endl;

        // Add includes
        for (const auto &include : includes) {
          clangd_file << "    \"-I"
                      << std::filesystem::absolute(include).string() << "\","
                      << std::endl;
        }

        // Add defines
        for (const auto &define : defines) {
          clangd_file << "    -D" << define << "," << std::endl;
        }

        // Add parsed flags
        for (const auto &flag : flags) {
          clangd_file << "    " << flag << "," << std::endl;
        }

        clangd_file << "  ]" << std::endl;
      };

  // 1. Write default block (C++ if available, else C, else whatever)
  write_flags(ExtractFlags(default_command), metadata->consolidated_includes,
              metadata->consolidated_defines);

  // 2. If C++ was used as default, and there is a C command, add conditional
  // block to handle .c files.
  if (!cpp_command.empty() && !c_command.empty()) {
    clangd_file << "---" << std::endl;
    clangd_file << "If:" << std::endl;
    clangd_file << "  PathMatch: .*\\.c" << std::endl;
    // Only pass flags for the conditional block (includes/defines are
    // inherited).
    write_flags(ExtractFlags(c_command));
  }

  clangd_file.close();
}

void GenerateClangdForPackages() {
  ForEachInputPackage([](const std::string &package_path) {
    MaybeGenerateClangdForPackage(GetPackageNameFromPath(package_path));
  });
}
