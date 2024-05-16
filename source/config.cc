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

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#include "execute.h"
#include "invocation.h"
#include "nlohmann/json.hpp"
#include "optimization_level.h"
#include "string_replace.h"
#include "temp_directory.h"
#include "timestamps.h"

using json = ::nlohmann::json;

namespace {

// The name of the config file.
constexpr char kConfigFile[] = ".rebs.jsonnet";
// The name of the config file for a package.
constexpr char kPackageConfigFile[] = ".package.rebs.jsonnet";
// The name of the generated json file.
constexpr char kConfigJsonFile[] = "rebs.json";
// The name of the generated concatinated jsonett file;
constexpr char kTempConcatinatedConfigFile[] = "temp.jsonnet";

// The default config file contents.
constexpr char kDefaultConfigFileContents[] = R"json(
local optimization_level = std.extVar("optimization_level");
{
  local cpp_compiler = "clang++",
  local archiver = "llvm-ar",
  "build_commands": {
    // C and C++:
    local c_optimizations =
      if optimization_level == "optimized" then
        " -g -O3 -fomit-frame-pointer -flto"
      else if optimization_level == "debug" then
        " -g -Og"
      else
        "",
    local cpp_command = cpp_compiler + c_optimizations +
      " -c -std=c++20 ${cdefines} ${cincludes} -MD -MF ${deps_out} -o ${out} ${in} ",

    "cc": cpp_command,
    "cpp": cpp_command,
    "c": cpp_compiler + c_optimizations +
      " -c -std=c17 ${cdefines} ${cincludes} -MD -MF ${deps_out} -o ${out} ${in}",

    // Intel ASM:
    "asm": cpp_compiler + c_optimizations + " -c -MD -MF ${deps_out} -o ${out} ${in}",

    // AT&T ASM:
    local att_asm = 'nasm -o ${out} ${in}',
    "s": att_asm,
    "S": att_asm
  },
  local application_linker_optimizations =
      if optimization_level == "optimized" then
        " -O3 -g -s --gc-sections"
      else " -g",
  "linker_command":
    if self.package_type == "application" then
      cpp_compiler + application_linker_optimizations + " -o ${out} ${in}"
    else if self.package_type == "library" then
      archiver + " rcs ${out} ${in}"
    else
      "",
  "output_extension":
    if self.package_type == "application" then
      ""
    else if self.package_type == "library" then
      "lib"
    else
      "",
  "source_directories": [
    ""
  ],
  "package_type": "application",
  "package_directories": [
${package_directories}
  ],
  "parallel_tasks" : ${parallel_tasks}
}

)json";

json global_config_file;

std::string temp_concatinated_jsonett_file;
std::string jsonet_command;

// The global config files concatinated together, with an extra "+" ready to
// append a local jsonet file onto the end.
std::string prepended_jsonet_configs;

std::vector<std::string> global_config_files;

// Timestamp of when the global configs changed.
uint64_t global_config_file_timestamp;

std::vector<std::filesystem::path> package_directories;
int number_of_parallel_tasks;

// Returns the user's home directory.
std::filesystem::path GetHomeDirectory() {
  // Check the POSIX home directory.
  const char* home_env = getenv("HOME");
  if (home_env != nullptr) return home_env;

  // Check the Windows home directory.
  home_env = getenv("USERPROFILE");
  if (home_env != nullptr) return home_env;

  // Fallback. This usually doesn't work but something is better than nothing.
  return "~";
}

// Populates the command used to call Jsonett.
void PopulateJsonnettCommand() {
  jsonet_command = std::vformat(
      "jsonnet --ext-str optimization_level=\"{}\"",
      std::make_format_args(OptimizationLevelToString(GetOptimizationLevel())));
}

// Gets the config file's path.
std::filesystem::path GetRootConfigFilePath() {
  const char* config_file = getenv("REBS_CONFIG");
  if (config_file != nullptr) return config_file;

  std::filesystem::path home_directory = GetHomeDirectory();
  return home_directory / kConfigFile;
}

// Creates and loads a default config file.
void CreateDefaultConfigFile(const std::string& config_file_path) {
  std::vector<std::string> package_directories;
  std::filesystem::path home_directory = GetHomeDirectory();
  for (const auto& sub_directory :
       {"applications", "libraries", "third_party"}) {
    package_directories.push_back(home_directory / "sources" / sub_directory);
  }

  std::stringstream package_directories_str;
  std::cout << "The default package directories are:" << std::endl;
  for (const auto& package_directory : package_directories) {
    std::cout << "  " << package_directory << std::endl;
    package_directories_str << "    " << std::quoted(package_directory.c_str())
                            << "," << std::endl;
  }

  std::string default_file_contents = kDefaultConfigFileContents;
  ReplaceSubstringInString(default_file_contents, "${package_directories}",
                           package_directories_str.str());
  ReplaceSubstringInString(default_file_contents, "${parallel_tasks}",
                           std::to_string(std::thread::hardware_concurrency()));

  std::ofstream file(config_file_path);
  if (file.is_open()) {
    std::cout << "Writing config file to " << config_file_path << std::endl;
    file << default_file_contents;
    file.close();
  } else {
    std::cerr << "Cannot write a config file to " << config_file_path
              << std::endl
              << "You can set the environment variable REBS_CONFIG to the path "
                 "you want to use."
              << std::endl;
  }

  // config_file = json::parse(default_file_contents);*/
  SetTimestampOfFileToNow(config_file_path);
}

// Returns the path to the root config file. Creates it if it does not exist.
std::string GetOrCreateRootConfigFile() {
  auto path = GetRootConfigFilePath();
  if (!DoesFileExist(path)) {
    CreateDefaultConfigFile(path);
  }
  return path;
}

// Returns a list of config files.
std::vector<std::string> GetGlobalConfigFiles() {
  // Use the root level one.
  std::vector<std::string> config_files = {GetOrCreateRootConfigFile()};

  // See if there's another config file at this level.
  if (DoesFileExist(kConfigFile)) config_files.push_back(kConfigFile);
  return config_files;
}

// Reads the global config files and concatinates them together.
std::string ReadAndConcatinateGlobalConfigFiles() {
  std::string concatinated_contents_string;
  bool first_file = true;
  for (const auto& config_file : global_config_files) {
    std::ifstream file(config_file);
    if (file.is_open()) {
      if (first_file)
        first_file = false;
      else
        concatinated_contents_string += "+";
      std::stringstream buffer;
      buffer << file.rdbuf();
      concatinated_contents_string += buffer.str();
      file.close();
    }
  }
  return concatinated_contents_string;
}

bool GenerateGlobalJsonFile(const std::string& generated_json_file) {
  std::string concatinated_contents_string =
      ReadAndConcatinateGlobalConfigFiles();

  std::filesystem::path temp_jsonnet_file =
      GetTempDirectoryPath() / kTempConcatinatedConfigFile;

  std::ofstream file(temp_jsonnet_file);
  if (file.is_open()) {
    file << concatinated_contents_string;
    file.close();
  }

  std::string command = std::vformat(
      "{} -o \"{}\" \"{}\"",
      std::make_format_args(jsonet_command, (std::string)generated_json_file,
                            (std::string)temp_jsonnet_file));
  if (!ExecuteCommand(command)) return false;
  SetTimestampOfFileToNow(generated_json_file);
  return true;
}

bool GenerateConfigFileForPackage(
    const std::filesystem::path& config_path,
    const std::filesystem::path& generated_config_path) {
  if (prepended_jsonet_configs.size() == 0) {
    prepended_jsonet_configs = ReadAndConcatinateGlobalConfigFiles() + "+";
  }

  std::filesystem::path temp_jsonnet_file =
      GetTempDirectoryPath() / kTempConcatinatedConfigFile;

  std::ofstream temp_file(temp_jsonnet_file);
  if (temp_file.is_open()) {
    temp_file << prepended_jsonet_configs;

    std::ifstream config_file(config_path);
    if (config_file.is_open()) {
      temp_file << config_file.rdbuf();
      config_file.close();
    }
    temp_file.close();
  }

  std::string command = std::vformat(
      "{} -o \"{}\" \"{}\"",
      std::make_format_args(jsonet_command, (std::string)generated_config_path,
                            (std::string)temp_jsonnet_file));
  if (!ExecuteCommand(command)) return false;
  SetTimestampOfFileToNow(generated_config_path);
  return true;
}

bool LoadGlobalConfigFile() {
  global_config_files = GetGlobalConfigFiles();

  // Calculate the last known timestamp.
  global_config_file_timestamp = 0;

  for (const auto& config_file : global_config_files) {
    global_config_file_timestamp =
        std::max(global_config_file_timestamp, GetTimestampOfFile(config_file));
  }

  std::filesystem::path generated_json_file =
      GetTempDirectoryPath() / kConfigJsonFile;

  if (global_config_file_timestamp > GetTimestampOfFile(generated_json_file)) {
    // One of the files is newer than the generated one.
    if (!GenerateGlobalJsonFile(generated_json_file)) return false;
  }

  std::ifstream file(generated_json_file);
  global_config_file = json::parse(file);
  file.close();
  return true;
}

void ParseGlobalConfig() {
  number_of_parallel_tasks =
      global_config_file["parallel_tasks"].template get<int>();
  for (const auto& directory : global_config_file["package_directories"]) {
    package_directories.push_back(directory.template get<std::string>());
  }
}

}  // namespace

bool LoadConfigFile() {
  PopulateJsonnettCommand();
  if (!LoadGlobalConfigFile()) return false;
  ParseGlobalConfig();
  return true;
}

bool IsThereALocalConfig() { return DoesFileExist(kConfigFile); }

int GetNumberOfParallelTasks() { return number_of_parallel_tasks; }

// Calls a function for each directory that may contain packages.
void ForEachPackageDirectory(
    const std::function<void(const std::filesystem::path&)>&
        on_each_directory) {
  for (const auto& directory : package_directories)
    on_each_directory(directory);
}

std::optional<::nlohmann::json> LoadConfigFileForPackage(
    const std::string& package_name, const std::filesystem::path& package_path,
    uint64_t& timestamp) {
  std::filesystem::path config_path = package_path / kPackageConfigFile;
  if (!DoesFileExist(config_path)) return global_config_file;

  timestamp =
      std::max(global_config_file_timestamp, GetTimestampOfFile(config_path));

  std::filesystem::path temp_path =
      GetTempDirectoryPathForPackagePath(package_path);
  EnsureDirectoriesAndParentsExist(temp_path);

  std::filesystem::path generated_config_path = temp_path / kPackageConfigFile;
  if (timestamp > GetTimestampOfFile(generated_config_path)) {
    // One of the files is newer than the generated one.
    if (!GenerateConfigFileForPackage(config_path, generated_config_path))
      return std::nullopt;
  }

  std::ifstream file(generated_config_path);
  json config = json::parse(file);
  file.close();
  return config;
}
