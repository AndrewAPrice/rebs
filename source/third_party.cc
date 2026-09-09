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

#include "third_party.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "execute.h"
#include "invocation.h"
#include "nlohmann/json.hpp"
#include "packages.h"
#include "string_replace.h"
#include "temp_directory.h"
#include "timestamps.h"

namespace {

using json = nlohmann::json;

// Replaces all instances of `from` with `to` in `str`.
std::string ReplaceAll(std::string str, const std::string& from,
                       const std::string& to) {
  if (from.empty()) return str;
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
  return str;
}
// Reads the entire contents of a file into a string.
std::optional<std::string> ReadFileToString(const std::filesystem::path& path) {
  std::ifstream f(path);
  if (!f.is_open()) return std::nullopt;
  std::stringstream buffer;
  buffer << f.rdbuf();
  return buffer.str();
}

// Writes content to a file, creating directories if needed, and invalidates
// timestamp.
bool WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  if (!out.is_open()) return false;
  out << content;
  out.close();
  InvalidateTimestamp(path.string());
  return true;
}

// --- Repositories ---

const char* kRepositoriesMapFile = "repositories.json";
const char* kThirdPartyFilesJson = ".third_party_files.json";

struct RepositoryMap {
  std::map<std::string, int> repositories_to_ids;
  int next_repository_id = 0;
  bool needs_flushing = false;
};

RepositoryMap global_repository_map;
bool repository_map_loaded = false;

// Returns the directory where third party temp files for a package are stored.
std::filesystem::path GetThirdPartyTempDirectory(
    const std::filesystem::path& package_path) {
  return GetTempDirectoryWithoutOptimizationLevelPath() / "third_party" /
         GetPackageNameFromPath(package_path);
}

// Returns the directory where repositories are cached.
std::filesystem::path GetRepositoriesDirectory() {
  return GetTempDirectoryWithoutOptimizationLevelPath() / "repositories";
}

// Returns the path to the repositories map file.
std::filesystem::path GetRepositoriesMapPath() {
  return GetRepositoriesDirectory() / kRepositoriesMapFile;
}

// Loads the repositories map from disk.
void LoadRepositoriesMap() {
  if (repository_map_loaded) return;

  std::filesystem::path map_path = GetRepositoriesMapPath();
  if (std::filesystem::exists(map_path)) {
    try {
      std::ifstream f(map_path);
      json j;
      f >> j;
      global_repository_map.repositories_to_ids =
          j["repositoriesToIds"].get<std::map<std::string, int>>();
      global_repository_map.next_repository_id =
          j["nextRepositoryId"].get<int>();
    } catch (const std::exception& e) {
      std::cerr << "Error reading " << map_path << ": " << e.what()
                << std::endl;
      // Fallback to empty map
    }
  } else {
    std::filesystem::create_directories(GetRepositoriesDirectory());
    global_repository_map.next_repository_id = 0;
  }
  repository_map_loaded = true;
}

// Flushes the repositories map to disk.
void FlushRepositoriesMap() {
  if (!global_repository_map.needs_flushing) return;

  std::filesystem::create_directories(GetRepositoriesDirectory());
  std::ofstream f(GetRepositoriesMapPath());
  json j;
  j["repositoriesToIds"] = global_repository_map.repositories_to_ids;
  j["nextRepositoryId"] = global_repository_map.next_repository_id;
  f << j.dump(4);
  global_repository_map.needs_flushing = false;
}

// Returns the directory for a specific repository key.
std::filesystem::path GetRepositoryDirectory(const std::string& key) {
  int repo_id;
  bool new_repo = false;
  if (global_repository_map.repositories_to_ids.find(key) ==
      global_repository_map.repositories_to_ids.end()) {
    repo_id = global_repository_map.next_repository_id++;
    global_repository_map.repositories_to_ids[key] = repo_id;
    global_repository_map.needs_flushing = true;
    new_repo = true;
  } else {
    repo_id = global_repository_map.repositories_to_ids[key];
  }

  std::filesystem::path dir =
      GetRepositoriesDirectory() / std::to_string(repo_id);

  if (new_repo && std::filesystem::exists(dir)) {
    std::filesystem::remove_all(dir);
  }
  return dir;
}

// --- Placeholders ---

struct PlaceholderInfo {
  std::map<std::string, std::vector<std::string>> placeholders;

  void Set(const std::string& name, const std::vector<std::string>& values) {
    std::string key = (name.rfind("${", 0) == 0 && name.back() == '}')
                          ? name
                          : "${" + name + "}";
    placeholders[key] = values;
  }

  void Set(const std::string& name, const std::string& value) {
    Set(name, std::vector<std::string>{value});
  }

  std::string GetFirst(const std::string& name,
                       const std::string& default_val = "") const {
    std::string key = (name.rfind("${", 0) == 0 && name.back() == '}')
                          ? name
                          : "${" + name + "}";
    auto it = placeholders.find(key);
    if (it != placeholders.end() && !it->second.empty()) {
      return it->second[0];
    }
    return default_val;
  }
};

// Substitutes placeholders in a string.
std::vector<std::string> SubstitutePlaceholdersInString(
    const std::string& str, const PlaceholderInfo& info) {
  std::vector<std::string> keys_found;
  for (const auto& [key, val] : info.placeholders) {
    if (str.find(key) != std::string::npos) {
      keys_found.push_back(key);
    }
  }

  if (keys_found.empty()) return {str};

  std::vector<std::string> results = {str};

  for (const auto& key : keys_found) {
    std::vector<std::string> next_results;
    const auto& values = info.placeholders.at(key);

    for (const auto& current_str : results) {
      for (const auto& val : values) {
        next_results.push_back(ReplaceAll(current_str, key, val));
      }
    }
    results = std::move(next_results);
  }

  return results;
}

// Substitutes placeholders in a vector of strings.
std::vector<std::string> SubstitutePlaceholders(
    const std::vector<std::string>& strs, const PlaceholderInfo& info) {
  std::vector<std::string> results;
  for (const auto& s : strs) {
    auto substituted = SubstitutePlaceholdersInString(s, info);
    results.insert(results.end(), substituted.begin(), substituted.end());
  }
  return results;
}

// Evaluates a single path relative to the package root if not absolute.
std::vector<std::string> EvaluatePath(const std::string& p,
                                      const PlaceholderInfo& info) {
  std::string raw_path = p;
  if (!raw_path.empty() && raw_path[0] != '$' &&
      !std::filesystem::path(raw_path).is_absolute()) {
    raw_path = "${@}/" + raw_path;
  }
  return SubstitutePlaceholdersInString(raw_path, info);
}

// Evaluates paths relative to the package root if not absolute.
std::vector<std::string> EvaluatePaths(const std::vector<std::string>& paths,
                                       const PlaceholderInfo& info) {
  std::vector<std::string> resolved_paths;
  for (const auto& p : paths) {
    auto expanded = EvaluatePath(p, info);
    resolved_paths.insert(resolved_paths.end(), expanded.begin(),
                          expanded.end());
  }
  return resolved_paths;
}

// Evaluates paths (vector overload for compatibility).
std::vector<std::string> EvaluatePath(const std::vector<std::string>& paths,
                                      const PlaceholderInfo& info) {
  return EvaluatePaths(paths, info);
}

// Converts a JSON object (string or array of strings) to a vector of strings.
std::vector<std::string> JsonToStringVector(const json& j) {
  if (j.is_string()) return {j.get<std::string>()};
  if (j.is_array()) {
    std::vector<std::string> v;
    for (const auto& el : j) {
      if (el.is_string()) v.push_back(el.get<std::string>());
    }
    return v;
  }
  return {};
}

// Returns a vector of strings from a JSON field, whether string, array, or
// missing.
std::vector<std::string> GetJsonStrings(const json& parent,
                                        const std::string& key) {
  auto it = parent.find(key);
  if (it != parent.end()) {
    return JsonToStringVector(*it);
  }
  return {};
}

// Evaluates paths from a JSON field.
std::vector<std::string> GetEvaluatedPaths(const json& parent,
                                           const std::string& key,
                                           const PlaceholderInfo& info) {
  return EvaluatePaths(GetJsonStrings(parent, key), info);
}

// Substitutes placeholders from a JSON field.
std::vector<std::string> GetSubstitutedStrings(const json& parent,
                                               const std::string& key,
                                               const PlaceholderInfo& info) {
  return SubstitutePlaceholders(GetJsonStrings(parent, key), info);
}

// Retrieves and substitutes a string from a JSON field, returning the first
// result or default.
std::string GetSubstitutedString(const json& parent, const std::string& key,
                                 const PlaceholderInfo& info,
                                 const std::string& default_val = "") {
  std::string raw = parent.value(key, "");
  if (raw.empty()) return default_val;
  auto subs = SubstitutePlaceholdersInString(raw, info);
  return subs.empty() ? default_val : subs[0];
}

// Parses replacement pairs from a JSON array with placeholder substitutions.
std::vector<std::pair<std::string, std::string>> ParseReplacements(
    const json& replacements_json, const PlaceholderInfo& info) {
  std::vector<std::pair<std::string, std::string>> replacements;
  if (!replacements_json.is_array()) return replacements;
  for (const auto& r : replacements_json) {
    if (r.is_array() && r.size() == 2 && r[0].is_string() && r[1].is_string()) {
      auto needles =
          SubstitutePlaceholdersInString(r[0].get<std::string>(), info);
      auto withs =
          SubstitutePlaceholdersInString(r[1].get<std::string>(), info);
      for (const auto& needle : needles) {
        for (const auto& with : withs) {
          replacements.push_back({needle, with});
        }
      }
    }
  }
  return replacements;
}

// Applies a series of replacements to a string.
std::string ApplyReplacements(
    std::string str,
    const std::vector<std::pair<std::string, std::string>>& replacements) {
  for (const auto& kv : replacements) {
    str = ReplaceAll(str, kv.first, kv.second);
  }
  return str;
}

// Parses the "extensions" field.
std::set<std::string> ParseExtensions(const json& op) {
  std::set<std::string> extensions;
  if (op.contains("extensions")) {
    for (const auto& ext : op["extensions"])
      extensions.insert(ext.get<std::string>());
  }
  return extensions;
}

// Checks if the given path matches the set of extensions (or if no extensions
// are specified).
bool MatchesExtensions(const std::filesystem::path& p,
                       const std::set<std::string>& extensions) {
  return extensions.empty() ||
         extensions.find(p.extension().string()) != extensions.end();
}

// --- Operations ---

// Executes a command using std::system, passing stdout/stderr through.
bool ExecuteSystemCommand(const std::string& command) {
  return std::system(command.c_str()) == 0;
}

// Downloads a file using curl to destination.
bool DownloadFile(const std::string& url,
                  const std::filesystem::path& destination) {
  std::filesystem::create_directories(destination.parent_path());
  std::cout << "Downloading " << url << std::endl;
  std::string cmd = "curl -L " + url + " --output " + destination.string();
  return ExecuteSystemCommand(cmd);
}

// Clones or updates a VCS repository.
bool SyncVcsRepository(const std::string& action_name,
                       const std::string& clone_cmd,
                       const std::string& update_cmd, const std::string& url,
                       const std::filesystem::path& dir) {
  if (std::filesystem::exists(dir)) {
    std::cout << "Updating " << url << std::endl;
    return ExecuteSystemCommand(update_cmd);
  } else {
    std::cout << action_name << " " << url << std::endl;
    return ExecuteSystemCommand(clone_cmd);
  }
}

// Loads a repository (git, zip, or download) and sets the placeholder.
bool LoadRepository(const json& repo_meta, PlaceholderInfo& info) {
  std::string type = repo_meta.value("type", "");
  std::string url = repo_meta.value("url", "");
  std::string placeholder = repo_meta.value("placeholder", "");

  if (type.empty() || url.empty() || placeholder.empty()) {
    std::cerr << "Invalid repository metadata." << std::endl;
    return false;
  }

  std::string key = type + "#" + url;
  std::filesystem::path dir = GetRepositoryDirectory(key);

  if (type == "download") {
    std::string filename = url.substr(url.find_last_of('/') + 1);
    std::filesystem::path file_path = dir / filename;
    if (!std::filesystem::exists(file_path)) {
      if (!DownloadFile(url, file_path)) return false;
    }
  } else if (type == "git") {
    if (!SyncVcsRepository("Cloning", "git clone " + url + " " + dir.string(),
                           "git -C " + dir.string() + " pull", url, dir))
      return false;
  } else if (type == "zip") {
    std::filesystem::path zip_path = dir / "download.zip";
    if (!std::filesystem::exists(zip_path)) {
      if (!DownloadFile(url, zip_path)) return false;
    }

    std::filesystem::path extracted_dir = dir / "extracted";
    if (!std::filesystem::exists(extracted_dir)) {
      if (!ExecuteSystemCommand("unzip " + zip_path.string() + " -d " +
                                extracted_dir.string()))
        return false;
    }
    dir = extracted_dir;
  } else if (type == "svn") {
    if (!SyncVcsRepository("Checking out",
                           "svn checkout " + url + " " + dir.string(),
                           "svn update " + dir.string(), url, dir))
      return false;
  } else {
    std::cerr << "Unknown repository type: " << type << std::endl;
    return false;
  }

  info.Set(placeholder, dir.string());
  return true;
}

// Copies a file from `from` to `to`, optionally using provided contents.
void CopyFile(const std::filesystem::path& from,
              const std::filesystem::path& to, const std::string& contents,
              bool use_contents,
              std::map<std::string, bool>& third_party_files) {
  third_party_files[to.string()] = true;

  // Check timestamps
  if (std::filesystem::exists(to) && !use_contents) {
    if (GetTimestampOfFile(from.string()) <= GetTimestampOfFile(to.string())) {
      return;
    }
  }

  if (use_contents) {
    WriteFile(to, contents);
  } else {
    std::filesystem::create_directories(to.parent_path());
    std::filesystem::copy_file(
        from, to, std::filesystem::copy_options::overwrite_existing);
    InvalidateTimestamp(to.string());
  }
  std::cout << "Copying " << to.string() << std::endl;
}

// Processes and copies a single file.
void CopyAndProcessFile(
    const std::filesystem::path& from, const std::filesystem::path& to,
    const std::map<std::string,
                   std::vector<std::pair<std::string, std::string>>>&
        replace_map,
    const std::map<std::string, std::string>& prepend_map,
    std::map<std::string, bool>& third_party_files) {
  bool needs_processing =
      replace_map.count(to.string()) || prepend_map.count(to.string());
  if (needs_processing) {
    auto content_opt = ReadFileToString(from);
    std::string content = content_opt.value_or("");

    if (prepend_map.count(to.string())) {
      content = prepend_map.at(to.string()) + content;
    }
    if (replace_map.count(to.string())) {
      content = ApplyReplacements(content, replace_map.at(to.string()));
    }
    CopyFile(from, to, content, true, third_party_files);
  } else {
    CopyFile(from, to, "", false, third_party_files);
  }
}

// Executes a copy operation.
bool ExecuteCopy(const json& op, PlaceholderInfo& info,
                 std::map<std::string, bool>& third_party_files) {
  auto sources = GetEvaluatedPaths(op, "source", info);
  auto dests = GetEvaluatedPaths(op, "destination", info);

  if (sources.size() != dests.size()) {
    std::cerr << "Source and destination count mismatch in copy operation."
              << std::endl;
    std::cerr << "Found " << sources.size() << " sources and " << dests.size()
              << " destinations. Sources:" << std::endl;
    for (const auto& source : sources) std::cerr << source << std::endl;

    std::cerr << "Destinations: " << std::endl;
    for (const auto& dest : dests) std::cerr << dest << std::endl;

    std::cerr << std::endl << "Operation: " << op.dump(4) << std::endl;
    return false;
  }

  std::map<std::string, std::string> rename_map;
  if (op.contains("rename")) {
    for (const auto& [key, val] : op["rename"].items()) {
      auto keys = EvaluatePath(key, info);
      auto vals = EvaluatePaths(JsonToStringVector(val), info);
      if (vals.empty()) continue;
      for (const auto& k : keys) {
        rename_map[k] = vals[0];
      }
    }
  }

  std::map<std::string, std::vector<std::pair<std::string, std::string>>>
      replace_map;
  std::map<std::string, std::string> prepend_map;

  if (op.contains("replace")) {
    for (const auto& rep : op["replace"]) {
      auto files = GetEvaluatedPaths(rep, "file", info);
      for (const auto& f : files) {
        if (rep.contains("replacements")) {
          auto reps = ParseReplacements(rep["replacements"], info);
          replace_map[f].insert(replace_map[f].end(), reps.begin(), reps.end());
        }
        if (rep.contains("prepend")) {
          std::string p = GetSubstitutedString(rep, "prepend", info);
          if (!p.empty()) prepend_map[f] = p;
        }
      }
    }
  }

  bool recursive = op.value("recursive", false);
  auto ex_paths = GetEvaluatedPaths(op, "exclude", info);
  std::set<std::string> excludes(ex_paths.begin(), ex_paths.end());
  std::set<std::string> extensions = ParseExtensions(op);

  auto ProcessFile = [&](const std::filesystem::path& from_path,
                         std::filesystem::path to_path) {
    if (!MatchesExtensions(from_path, extensions)) return;

    if (rename_map.count(to_path.string())) {
      to_path = rename_map.at(to_path.string());
    }

    if (excludes.count(to_path.string())) return;

    CopyAndProcessFile(from_path, to_path, replace_map, prepend_map,
                       third_party_files);
  };

  for (size_t i = 0; i < sources.size(); ++i) {
    std::filesystem::path from = sources[i];
    std::filesystem::path to = dests[i];

    if (!std::filesystem::exists(from)) {
      std::cerr << "Source does not exist: " << from << std::endl;
      return false;
    }

    if (std::filesystem::is_directory(from)) {
      for (auto it = std::filesystem::recursive_directory_iterator(from);
           it != std::filesystem::recursive_directory_iterator(); ++it) {
        const auto& p = *it;
        if (p.is_directory()) {
          if (!recursive) it.disable_recursion_pending();
          continue;
        }

        std::filesystem::path rel = std::filesystem::relative(p.path(), from);
        ProcessFile(p.path(), to / rel);
      }
    } else {
      ProcessFile(from, to);
    }
  }
  return true;
}

// Executes a createDirectory operation.
bool ExecuteCreateDirectory(const json& op, PlaceholderInfo& info,
                            std::map<std::string, bool>& third_party_files) {
  auto paths = GetEvaluatedPaths(op, "path", info);
  for (const auto& p : paths) {
    std::filesystem::create_directories(p);
    InvalidateTimestamp(p);
  }
  return true;
}

// Executes an evaluate operation.
// Evaluates a single expression using python3.
std::optional<std::string> EvaluateExpressionString(const std::string& expr) {
  // We need to escape double quotes in the expression because we wrap it in "".
  std::string escaped_expr = ReplaceAll(expr, "\"", "\\\"");

  std::stringstream output_ss;
  std::string cmd = "python3 -c \"print(" + escaped_expr + ")\"";
  if (!ExecuteCommand(cmd, &output_ss)) {
    std::cerr << "Failed to evaluate: " << expr << std::endl;
    return std::nullopt;
  }

  std::string result = output_ss.str();
  // Trim newline
  if (!result.empty() && result.back() == '\n') result.pop_back();
  return result;
}

// Executes an evaluate operation.
bool ExecuteEvaluate(const json& op, PlaceholderInfo& info,
                     std::map<std::string, bool>& third_party_files) {
  if (op.contains("values")) {
    for (const auto& [key, val] : op["values"].items()) {
      std::vector<std::string> results;
      auto raw_expressions =
          SubstitutePlaceholders(JsonToStringVector(val), info);

      for (const auto& expr : raw_expressions) {
        auto result = EvaluateExpressionString(expr);
        if (!result.has_value()) return false;
        results.push_back(result.value());
      }

      info.Set(key, results);
    }
  }
  return true;
}

// Executes an execute operation.
bool ExecuteExecute(const json& op, PlaceholderInfo& info,
                    std::map<std::string, bool>& third_party_files) {
  long long newest_input = -1;
  long long oldest_output = -2;  // Sentinel for infinity concept
  bool missing_output = false;

  auto inputs = GetEvaluatedPaths(op, "inputs", info);
  for (const auto& input : inputs) {
    if (!std::filesystem::exists(input)) {
      std::cerr << "Input does not exist: " << input << std::endl;
      return false;
    }
    long long ts = GetTimestampOfFile(input);
    if (ts > newest_input) newest_input = ts;
  }

  auto final_outputs = GetEvaluatedPaths(op, "outputs", info);
  for (const auto& output : final_outputs) {
    third_party_files[output] = true;
    if (std::filesystem::exists(output)) {
      long long ts = GetTimestampOfFile(output);
      if (oldest_output == -2 || ts < oldest_output) oldest_output = ts;
    } else {
      missing_output = true;
      std::filesystem::create_directories(
          std::filesystem::path(output).parent_path());
    }
  }

  bool always_run = op.value("alwaysRun", false);
  if (!missing_output && oldest_output != -2 && newest_input < oldest_output &&
      !always_run && newest_input != -1) {
    return true;
  }

  for (const auto& output : final_outputs) {
    if (std::filesystem::exists(output)) {
      std::filesystem::remove(output);
      InvalidateTimestamp(output);
    }
  }

  std::string command = GetSubstitutedString(op, "command", info);
  if (command.empty()) return false;

  std::string cwd = GetSubstitutedString(op, "directory", info);
  std::string final_cmd =
      cwd.empty() ? command : "cd " + cwd + " && " + command;
  std::cout << "Executing: " << final_cmd << std::endl;
  if (!ExecuteSystemCommand(final_cmd)) return false;

  for (const auto& output : final_outputs) {
    InvalidateTimestamp(output);
  }

  return true;
}

// Executes a joinArray operation.
bool ExecuteJoinArray(const json& op, PlaceholderInfo& info,
                      std::map<std::string, bool>& third_party_files) {
  auto processed_values =
      SubstitutePlaceholders(GetJsonStrings(op, "value"), info);
  std::string joint_val = GetSubstitutedString(op, "joint", info);

  std::stringstream ss;
  for (size_t i = 0; i < processed_values.size(); i++) {
    if (i > 0) ss << joint_val;
    ss << processed_values[i];
  }

  std::string placeholder_name = op.value("placeholder", "");
  info.Set(placeholder_name, ss.str());
  return true;
}

// Executes a readFilesInDirectory operation.
bool ExecuteReadFilesInDirectory(
    const json& op, PlaceholderInfo& info,
    std::map<std::string, bool>& third_party_files) {
  auto paths = GetSubstitutedStrings(op, "path", info);

  std::vector<std::string> files_found;
  bool full_path = op.value("fullPath", false);
  std::set<std::string> extensions = ParseExtensions(op);

  std::optional<std::regex> filename_regex;
  if (op.contains("regex")) {
    try {
      filename_regex = std::regex(op["regex"].get<std::string>());
    } catch (const std::regex_error& e) {
      std::cerr << "Invalid regex: " << op["regex"].get<std::string>() << " - "
                << e.what() << std::endl;
      return false;
    }
  }

  auto replacements =
      ParseReplacements(op.value("replacements", json::array()), info);

  for (const auto& dir : paths) {
    if (!std::filesystem::exists(dir)) {
      std::cerr << "Directory does not exist: " << dir << std::endl;
      return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_directory()) continue;
      if (!MatchesExtensions(entry.path(), extensions)) continue;

      if (filename_regex.has_value()) {
        if (!std::regex_match(entry.path().filename().string(),
                              filename_regex.value())) {
          continue;
        }
      }

      std::string val =
          full_path ? entry.path().string() : entry.path().filename().string();

      val = ApplyReplacements(val, replacements);
      files_found.push_back(val);
    }
  }

  std::string placeholder_name = op.value("placeholder", "");
  info.Set(placeholder_name, files_found);
  return true;
}

// Executes a readRegExFromFile operation.
bool ExecuteReadRegExFromFile(const json& op, PlaceholderInfo& info,
                              std::map<std::string, bool>& third_party_files) {
  auto file_paths = GetEvaluatedPaths(op, "file", info);
  if (file_paths.empty()) return false;
  std::string path = file_paths[0];

  if (!std::filesystem::exists(path)) {
    std::cerr << "File does not exist: " << path << std::endl;
    return false;
  }

  auto content_opt = ReadFileToString(path);
  if (!content_opt.has_value()) {
    std::cerr << "Failed to read file: " << path << std::endl;
    return false;
  }
  const std::string& content = *content_opt;

  if (op.contains("values")) {
    for (auto& [key_list, regex_str] : op["values"].items()) {
      std::regex re(regex_str.get<std::string>());
      std::smatch match;
      if (std::regex_search(content, match, re)) {
        std::stringstream ss(key_list);
        std::string segment;
        int idx = 0;
        while (std::getline(ss, segment, ',')) {
          if (idx < match.size()) {
            if (!segment.empty()) {
              info.Set(segment, match[idx].str());
            }
          }
          idx++;
        }
      } else {
        std::cerr << "Regex not found in file: " << path << std::endl;
        std::cerr << "Regex: " << regex_str.get<std::string>() << std::endl;
        return false;
      }
    }
  }
  return true;
}

// Executes a set operation.
bool ExecuteSet(const json& op, PlaceholderInfo& info,
                std::map<std::string, bool>& third_party_files) {
  if (op.contains("values")) {
    for (const auto& [key, val] : op["values"].items()) {
      info.Set(key, SubstitutePlaceholders(JsonToStringVector(val), info));
    }
  }
  return true;
}

// Executes a dirname operation.
bool ExecuteDirname(const json& op, PlaceholderInfo& info,
                    std::map<std::string, bool>& third_party_files) {
  std::string placeholder = op.value("placeholder", "");
  if (placeholder.empty()) {
    std::cerr << "dirname operation missing 'placeholder'." << std::endl;
    return false;
  }

  std::string raw_path = op.value("path", "");
  auto paths = SubstitutePlaceholdersInString(raw_path, info);
  if (paths.empty() || paths[0].empty()) {
    std::cerr << "dirname operation has empty path." << std::endl;
    return false;
  }

  int depth = op.value("depth", 1);
  std::vector<std::string> results;
  for (const auto& p_str : paths) {
    std::filesystem::path p = std::filesystem::path(p_str).lexically_normal();
    for (int i = 0; i < depth; ++i) {
      p = p.parent_path();
    }
    results.push_back(p.string());
  }

  info.Set(placeholder, results);
  return true;
}

// Returns true if the path points to a regular file (or symlink) that is
// executable.
bool IsExecutableFile(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) return false;
#ifdef _WIN32
  return true;
#else
  auto perms = std::filesystem::status(p, ec).permissions();
  if (ec) return false;
  return (perms & (std::filesystem::perms::owner_exec |
                   std::filesystem::perms::group_exec |
                   std::filesystem::perms::others_exec)) !=
         std::filesystem::perms::none;
#endif
}

// Returns true if a search path entry specifies the system PATH.
bool IsPathEnvEntry(const std::string& entry) {
  return entry == "PATH" || entry == "$PATH" || entry == "${PATH}";
}

// Executes a findFile operation.
bool ExecuteFindFile(const json& op, PlaceholderInfo& info,
                     std::map<std::string, bool>& third_party_files) {
  std::string placeholder = op.value("placeholder", "");
  if (placeholder.empty()) {
    std::cerr << "findFile operation missing 'placeholder'." << std::endl;
    return false;
  }

  bool executable = op.value("executable", false);
  bool required = op.value("required", true);

  std::vector<std::string> names = GetJsonStrings(op, "names");
  if (names.empty()) {
    names = GetJsonStrings(op, "name");
  }
  names = SubstitutePlaceholders(names, info);

  std::vector<std::string> search_paths = GetJsonStrings(op, "searchPaths");
  if (search_paths.empty()) {
    search_paths = {"PATH"};
  }

  // Parse PATH environment variable directories once if needed
  std::vector<std::filesystem::path> env_path_dirs;
  bool parsed_env_path = false;
  auto GetEnvPathDirs = [&]() -> const std::vector<std::filesystem::path>& {
    if (!parsed_env_path) {
      const char* env_path = std::getenv("PATH");
      if (env_path) {
#ifdef _WIN32
        constexpr char kPathDelimiter = ';';
#else
        constexpr char kPathDelimiter = ':';
#endif
        std::stringstream ss(env_path);
        std::string dir_str;
        while (std::getline(ss, dir_str, kPathDelimiter)) {
          if (!dir_str.empty()) env_path_dirs.push_back(dir_str);
        }
      }
      parsed_env_path = true;
    }
    return env_path_dirs;
  };

  auto MatchesCriteria = [&](const std::filesystem::path& p) -> bool {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return false;
    if (std::filesystem::is_directory(p, ec)) return false;
    if (executable) {
      return IsExecutableFile(p);
    }
    return true;
  };

  std::vector<std::string> attempted_paths;
  std::string found_path;

  for (const auto& raw_entry : search_paths) {
    std::vector<std::filesystem::path> dirs_to_search;
    if (IsPathEnvEntry(raw_entry)) {
      dirs_to_search = GetEnvPathDirs();
    } else {
      auto evaluated_entries = EvaluatePath(raw_entry, info);
      for (const auto& entry : evaluated_entries) {
        dirs_to_search.push_back(entry);
      }
    }

    for (const auto& dir : dirs_to_search) {
      if (names.empty()) {
        attempted_paths.push_back(dir.string());
        if (MatchesCriteria(dir)) {
          found_path = dir.lexically_normal().string();
          break;
        }
      } else {
        for (const auto& name : names) {
          auto candidate = dir / name;
          attempted_paths.push_back(candidate.string());
          if (MatchesCriteria(candidate)) {
            found_path = candidate.lexically_normal().string();
            break;
          }
        }
        if (!found_path.empty()) break;
      }
    }
    if (!found_path.empty()) break;
  }

  if (!found_path.empty()) {
    info.Set(placeholder, found_path);
    return true;
  }

  if (required) {
    std::cerr << "Could not find file" << (executable ? " (executable)" : "")
              << " for placeholder '" << placeholder << "'." << std::endl;
    if (!attempted_paths.empty()) {
      std::cerr << "Attempted paths:" << std::endl;
      for (const auto& p : attempted_paths) {
        std::cerr << "  " << p << std::endl;
      }
    }
    return false;
  }

  return true;
}

// Executes a writeFile operation.
bool ExecuteWriteFile(const json& op, PlaceholderInfo& info,
                      std::map<std::string, bool>& third_party_files) {
  std::string raw_path = op.value("path", "");
  if (raw_path.empty()) {
    std::cerr << "writeFile operation missing 'path'." << std::endl;
    return false;
  }
  auto paths = EvaluatePath(raw_path, info);
  if (paths.empty()) {
    std::cerr << "writeFile could not resolve path: " << raw_path << std::endl;
    return false;
  }
  std::filesystem::path target(paths[0]);

  std::string content;
  if (op.contains("content")) {
    if (op["content"].is_string()) {
      content = op["content"].get<std::string>();
    } else if (op["content"].is_array()) {
      for (const auto& line : op["content"]) {
        if (line.is_string()) {
          std::string s = line.get<std::string>();
          content += s;
          if (s.empty() || s.back() != '\n') content += '\n';
        }
      }
    }
  }
  auto contents = SubstitutePlaceholdersInString(content, info);
  std::string final_content = contents.empty() ? content : contents[0];

  auto existing = ReadFileToString(target);
  bool needs_write = !existing.has_value() || (*existing != final_content);

  if (needs_write) {
    if (!WriteFile(target, final_content)) {
      std::cerr << "Failed to open file for writing: " << target << std::endl;
      return false;
    }
    std::cout << "Wrote " << target.string() << std::endl;
  }

  // If the path is within the package and not in temp, record it in
  // third_party_files
  std::string temp_dir_str = info.GetFirst("temp");
  std::string pkg_dir_str = info.GetFirst("@");

  std::string target_str = target.string();
  bool is_in_temp =
      (!temp_dir_str.empty() && target_str.rfind(temp_dir_str, 0) == 0);
  bool is_in_pkg =
      (!pkg_dir_str.empty() && target_str.rfind(pkg_dir_str, 0) == 0);

  if (is_in_pkg && !is_in_temp) {
    third_party_files[target_str] = true;
  }

  return true;
}

// Dispatches to the appropriate operation executor.
bool ExecuteOperation(const json& op, PlaceholderInfo& info,
                      std::map<std::string, bool>& third_party_files) {
  static const std::map<std::string,
                        std::function<bool(const json&, PlaceholderInfo&,
                                           std::map<std::string, bool>&)>>
      operations = {
          {"copy", ExecuteCopy},
          {"createDirectory", ExecuteCreateDirectory},
          {"dirname", ExecuteDirname},
          {"evaluate", ExecuteEvaluate},
          {"execute", ExecuteExecute},
          {"findFile", ExecuteFindFile},
          {"joinArray", ExecuteJoinArray},
          {"readFilesInDirectory", ExecuteReadFilesInDirectory},
          {"readRegExFromFile", ExecuteReadRegExFromFile},
          {"set", ExecuteSet},
          {"writeFile", ExecuteWriteFile},
      };

  std::string type = op.value("operation", "");
  auto it = operations.find(type);
  if (it != operations.end()) {
    return it->second(op, info, third_party_files);
  }

  std::cerr << "Unknown operation: " << type << std::endl;
  return false;
}

}  // namespace

// Updates third party packages for the given package.
bool UpdateThirdParty(const std::filesystem::path& package_path, bool force) {
  std::filesystem::path third_party_json = package_path / "third_party.json";
  std::filesystem::path third_party_files_json =
      package_path / kThirdPartyFilesJson;

  if (!std::filesystem::exists(third_party_json)) return true;  // Nothing to do

  if (!force) {
    // Check timestamps
    long long tp_time = GetTimestampOfFile(third_party_json.string());
    long long tpf_time =
        std::filesystem::exists(third_party_files_json)
            ? GetTimestampOfFile(third_party_files_json.string())
            : 0;

    if (tpf_time >= tp_time) return true;  // Up to date
  }

  CleanThirdParty(package_path);

  std::cout << "Updating third party packages for "
            << GetPackageNameFromPath(package_path) << "..." << std::endl;

  std::ifstream f(third_party_json);
  json config;
  try {
    f >> config;
  } catch (const std::exception& e) {
    std::cerr << "Failed to parse " << third_party_json << ": " << e.what()
              << std::endl;
    return false;
  }

  PlaceholderInfo info;
  info.Set("@", package_path.string());

  std::filesystem::path temp_directory =
      GetThirdPartyTempDirectory(package_path);
  std::filesystem::create_directories(temp_directory);
  info.Set("temp", temp_directory.string());

  LoadRepositoriesMap();

  if (config.contains("repositories")) {
    for (const auto& repo : config["repositories"]) {
      if (!LoadRepository(repo, info)) return false;
    }
  }
  FlushRepositoriesMap();

  std::map<std::string, bool> third_party_files;

  if (config.contains("operations")) {
    for (const auto& op : config["operations"]) {
      if (!ExecuteOperation(op, info, third_party_files)) return false;
    }
  }

  // Write .third_party_files.json
  json output_files = third_party_files;
  std::ofstream out(third_party_files_json);
  out << output_files.dump(4);

  return true;
}

bool MaybeUpdateThirdPartyBeforeBuilding(
    const std::filesystem::path& package_path) {
  bool force = ShouldUpdateThirdParty();
  if (force) return UpdateThirdParty(package_path, force);

  if (std::filesystem::exists(package_path / "third_party.json"))
    return UpdateThirdParty(package_path, false);

  return true;
}

// Updates third party packages.
bool UpdateThirdPartyPackages() {
  bool success = true;
  ForEachInputPackage([&](const std::string& package_path_str) {
    success &= UpdateThirdParty(package_path_str, true);
  });
  return success;
}

bool CleanThirdParty(const std::filesystem::path& package_path) {
  std::filesystem::path temp_directory =
      GetThirdPartyTempDirectory(package_path);
  if (std::filesystem::exists(temp_directory)) {
    std::filesystem::remove_all(temp_directory);
  }

  std::filesystem::path third_party_files_json =
      package_path / kThirdPartyFilesJson;
  if (!std::filesystem::exists(third_party_files_json)) return true;

  try {
    std::ifstream f(third_party_files_json);
    json j;
    f >> j;
    for (auto& [path_str, val] : j.items()) {
      if (std::filesystem::exists(path_str)) {
        std::filesystem::remove(path_str);
      }
    }
    std::filesystem::remove(third_party_files_json);
  } catch (const std::exception& e) {
    std::cerr << "Error cleaning third party: " << e.what() << std::endl;
    return false;
  }
  return true;
}

bool CleanRepositoriesDirectory() {
  std::filesystem::path repo_dir = GetRepositoriesDirectory();
  if (std::filesystem::exists(repo_dir)) {
    std::cout << "Cleaning repositories directory: " << repo_dir << std::endl;
    std::filesystem::remove_all(repo_dir);
  }
  global_repository_map = {};
  repository_map_loaded = false;
  return true;
}
