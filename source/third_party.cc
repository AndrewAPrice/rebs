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
std::string ReplaceAll(std::string str, const std::string &from,
                       const std::string &to) {
  if (from.empty())
    return str;
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
  return str;
}

// --- Repositories ---

const char *kRepositoriesMapFile = "repositories.json";

struct RepositoryMap {
  std::map<std::string, int> repositories_to_ids;
  int next_repository_id = 0;
  bool needs_flushing = false;
};

RepositoryMap global_repository_map;
bool repository_map_loaded = false;

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
  if (repository_map_loaded)
    return;

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
    } catch (const std::exception &e) {
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
  if (!global_repository_map.needs_flushing)
    return;

  std::filesystem::create_directories(GetRepositoriesDirectory());
  std::ofstream f(GetRepositoriesMapPath());
  json j;
  j["repositoriesToIds"] = global_repository_map.repositories_to_ids;
  j["nextRepositoryId"] = global_repository_map.next_repository_id;
  f << j.dump(4);
  global_repository_map.needs_flushing = false;
}

// Returns the directory for a specific repository key.
std::filesystem::path GetRepositoryDirectory(const std::string &key) {
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
};

// Substitutes placeholders in a string.
std::vector<std::string>
SubstitutePlaceholdersInString(const std::string &str,
                               const PlaceholderInfo &info) {
  std::vector<std::string> keys_found;
  for (const auto &[key, val] : info.placeholders) {
    if (str.find(key) != std::string::npos) {
      keys_found.push_back(key);
    }
  }

  if (keys_found.empty())
    return {str};

  std::vector<std::string> results = {str};

  for (const auto &key : keys_found) {
    std::vector<std::string> next_results;
    const auto &values = info.placeholders.at(key);

    for (const auto &current_str : results) {
      for (const auto &val : values) {
        std::string s = current_str;
        size_t pos = 0;
        while ((pos = s.find(key, pos)) != std::string::npos) {
          s.replace(pos, key.length(), val);
          pos += val.length();
        }
        next_results.push_back(s);
      }
    }
    results = next_results;
  }

  return results;
}

// Substitutes placeholders in a vector of strings.
std::vector<std::string>
SubstitutePlaceholders(const std::vector<std::string> &strs,
                       const PlaceholderInfo &info) {
  std::vector<std::string> results;
  for (const auto &s : strs) {
    auto substituted = SubstitutePlaceholdersInString(s, info);
    results.insert(results.end(), substituted.begin(), substituted.end());
  }
  return results;
}

// Evaluates a path relative to the package root if not absolute.
std::vector<std::string> EvaluatePath(const std::vector<std::string> &paths,
                                      const PlaceholderInfo &info) {
  std::vector<std::string> resolved_paths;

  auto pkg_root_it = info.placeholders.find("${@}");
  std::string pkg_root =
      (pkg_root_it != info.placeholders.end() && !pkg_root_it->second.empty())
          ? pkg_root_it->second[0]
          : ".";

  for (const auto &p : paths) {
    std::string raw_path = p;
    if (raw_path.empty() || raw_path[0] != '$') {
      raw_path = "${@}/" + raw_path;
    }
    auto expanded = SubstitutePlaceholdersInString(raw_path, info);
    resolved_paths.insert(resolved_paths.end(), expanded.begin(),
                          expanded.end());
  }
  return resolved_paths;
}

// Converts a JSON object (string or array of strings) to a vector of strings.
std::vector<std::string> JsonToStringVector(const json &j) {
  if (j.is_string())
    return {j.get<std::string>()};
  if (j.is_array()) {
    std::vector<std::string> v;
    for (const auto &el : j) {
      if (el.is_string())
        v.push_back(el.get<std::string>());
    }
    return v;
  }
  return {};
}

// --- Operations ---

// Executes a command using std::system, passing stdout/stderr through.
bool ExecuteSystemCommand(const std::string &command) {
  return std::system(command.c_str()) == 0;
}

// Loads a repository (git, zip, or download) and sets the placeholder.
bool LoadRepository(const json &repo_meta, PlaceholderInfo &info) {
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
    std::filesystem::create_directories(dir);
    std::string filename = url.substr(url.find_last_of('/') + 1);
    std::filesystem::path file_path = dir / filename;
    if (!std::filesystem::exists(file_path)) {
      std::cout << "Downloading " << url << std::endl;
      std::string cmd = "curl -L " + url + " --output " + file_path.string();
      if (!ExecuteSystemCommand(cmd))
        return false;
    }
  } else if (type == "git") {
    if (std::filesystem::exists(dir)) {
      std::cout << "Updating " << url << std::endl;
      if (!ExecuteSystemCommand("git -C " + dir.string() + " pull"))
        return false;
    } else {
      std::cout << "Cloning " << url << std::endl;
      if (!ExecuteSystemCommand("git clone " + url + " " + dir.string()))
        return false;
    }
  } else if (type == "zip") {
    std::filesystem::path base_dir = dir;
    std::filesystem::create_directories(base_dir);
    std::filesystem::path zip_path = base_dir / "download.zip";

    if (!std::filesystem::exists(zip_path)) {
      std::cout << "Downloading " << url << std::endl;
      if (!ExecuteSystemCommand("curl -L " + url + " --output " +
                                zip_path.string()))
        return false;
    }

    std::filesystem::path extracted_dir = base_dir / "extracted";
    if (!std::filesystem::exists(extracted_dir)) {
      if (!ExecuteSystemCommand("unzip " + zip_path.string() + " -d " +
                                extracted_dir.string()))
        return false;
    }
    dir = extracted_dir;
  } else {
    std::cerr << "Unknown repository type: " << type << std::endl;
    return false;
  }

  info.placeholders["${" + placeholder + "}"] = {dir.string()};
  return true;
}

// Copies a file from `from` to `to`, optionally using provided contents.
void CopyFile(const std::filesystem::path &from,
              const std::filesystem::path &to, const std::string &contents,
              bool use_contents,
              std::map<std::string, bool> &third_party_files) {
  std::filesystem::create_directories(to.parent_path());
  third_party_files[to.string()] = true;

  // Check timestamps
  if (std::filesystem::exists(to) && !use_contents) {
    if (GetTimestampOfFile(from.string()) <= GetTimestampOfFile(to.string())) {
      return;
    }
  }

  if (use_contents) {
    std::ofstream out(to);
    out << contents;
  } else {
    std::filesystem::copy_file(
        from, to, std::filesystem::copy_options::overwrite_existing);
  }
  std::cout << "Copying " << to.string() << std::endl;
}

// Executes a copy operation.
bool ExecuteCopy(const json &op, PlaceholderInfo &info,
                 std::map<std::string, bool> &third_party_files) {
  auto sources = EvaluatePath(JsonToStringVector(op["source"]), info);
  auto dests = EvaluatePath(JsonToStringVector(op["destination"]), info);

  if (sources.size() != dests.size()) {
    std::cerr << "Source and destination count mismatch in copy operation."
              << std::endl;
    std::cerr << "Found " << sources.size() << " sources and " << dests.size()
              << " destinations. Sources:" << std::endl;
    for (const auto &source : sources)
      std::cerr << source << std::endl;

    std::cerr << "Destinations: " << std::endl;
    for (const auto &dest : dests)
      std::cerr << dest << std::endl;

    std::cerr << std::endl << "Operation: " << op.dump(4) << std::endl;
    return false;
  }

  std::map<std::string, std::vector<std::pair<std::string, std::string>>>
      replace_map;
  std::map<std::string, std::string> prepend_map;

  if (op.contains("replace")) {
    for (const auto &rep : op["replace"]) {
      auto files = EvaluatePath(JsonToStringVector(rep["file"]), info);
      for (const auto &f : files) {
        if (rep.contains("replacements")) {
          for (const auto &r : rep["replacements"]) {
            if (r.size() == 2) {
              auto needles = SubstitutePlaceholdersInString(r[0], info);
              auto w = SubstitutePlaceholdersInString(r[1], info);
              for (const auto &needle : needles) {
                for (const auto &with : w) {
                  replace_map[f].push_back({needle, with});
                }
              }
            }
          }
        }
        if (rep.contains("prepend")) {
          auto p = SubstitutePlaceholdersInString(rep["prepend"], info);
          if (!p.empty())
            prepend_map[f] = p[0];
        }
      }
    }
  }

  bool recursive = op.value("recursive", false);
  std::set<std::string> excludes;
  if (op.contains("exclude")) {
    auto ex_paths = EvaluatePath(JsonToStringVector(op["exclude"]), info);
    for (const auto &p : ex_paths)
      excludes.insert(p);
  }

  for (size_t i = 0; i < sources.size(); ++i) {
    std::filesystem::path from = sources[i];
    std::filesystem::path to = dests[i];

    if (!std::filesystem::exists(from)) {
      std::cerr << "Source does not exist: " << from << std::endl;
      return false;
    }

    if (std::filesystem::is_directory(from)) {
      for (auto &p : std::filesystem::recursive_directory_iterator(from)) {
        if (p.is_directory() && !recursive)
          continue;
        if (p.is_directory())
          continue;

        std::filesystem::path rel = std::filesystem::relative(p.path(), from);
        std::filesystem::path dest_file = to / rel;

        if (excludes.count(dest_file.string()))
          continue;

        bool needs_processing = replace_map.count(dest_file.string()) ||
                                prepend_map.count(dest_file.string());

        if (needs_processing) {
          std::ifstream t(p.path());
          std::stringstream buffer;
          buffer << t.rdbuf();
          std::string content = buffer.str();

          if (prepend_map.count(dest_file.string())) {
            content = prepend_map[dest_file.string()] + content;
          }
          if (replace_map.count(dest_file.string())) {
            for (auto &kv : replace_map[dest_file.string()]) {
              content = ReplaceAll(content, kv.first, kv.second);
            }
          }
          CopyFile(p.path(), dest_file, content, true, third_party_files);
        } else {
          CopyFile(p.path(), dest_file, "", false, third_party_files);
        }
      }
    } else {
      bool needs_processing =
          replace_map.count(to.string()) || prepend_map.count(to.string());
      if (needs_processing) {
        std::ifstream t(from);
        std::stringstream buffer;
        buffer << t.rdbuf();
        std::string content = buffer.str();

        if (prepend_map.count(to.string())) {
          content = prepend_map[to.string()] + content;
        }
        if (replace_map.count(to.string())) {
          for (auto &kv : replace_map[to.string()]) {
            content = ReplaceAll(content, kv.first, kv.second);
          }
        }
        CopyFile(from, to, content, true, third_party_files);
      } else {
        CopyFile(from, to, "", false, third_party_files);
      }
    }
  }
  return true;
}

// Executes a createDirectory operation.
bool ExecuteCreateDirectory(const json &op, PlaceholderInfo &info,
                            std::map<std::string, bool> &third_party_files) {
  auto paths = EvaluatePath(JsonToStringVector(op["path"]), info);
  for (const auto &p : paths) {
    std::filesystem::create_directories(p);
  }
  return true;
}

// Executes an evaluate operation.
// Evaluates a single expression using python3.
std::string EvaluateExpressionString(const std::string &expr) {
  // Use python3 to evaluate.
  // We need to escape single quotes slightly?
  // The expression is passed inside "print(eval('<expr>'))"
  // Actually, we can just pass the expression directly to eval if we handle
  // quoting. Safer: pass as command line arg? Simplest: python3 -c
  // "print(<expr>)" But <expr> might contain quotes. Let's assume the
  // expression is valid JS/Python "math + string concat". Example: '1'+'.'+'2'
  // python3 -c "print('1'+'.'+'2')" works.

  // We need to escape double quotes in the expression because we wrap it in "".
  std::string escaped_expr = ReplaceAll(expr, "\"", "\\\"");

  std::stringstream output_ss;
  std::string cmd = "python3 -c \"print(" + escaped_expr + ")\"";
  if (!ExecuteCommand(cmd, &output_ss)) {
    std::cerr << "Failed to evaluate: " << expr << std::endl;
    return expr; // Fallback? or empty?
  }

  std::string result = output_ss.str();
  // Trim newline
  if (!result.empty() && result.back() == '\n')
    result.pop_back();
  return result;
}

// Executes an evaluate operation.
bool ExecuteEvaluate(const json &op, PlaceholderInfo &info,
                     std::map<std::string, bool> &third_party_files) {
  if (op.contains("values")) {
    for (const auto &[key, val] : op["values"].items()) {
      std::vector<std::string> results;
      std::vector<std::string> raw_expressions;

      if (val.is_string()) {
        raw_expressions =
            SubstitutePlaceholdersInString(val.get<std::string>(), info);
      } else if (val.is_array()) {
        std::vector<std::string> raws = JsonToStringVector(val);
        raw_expressions = SubstitutePlaceholders(raws, info);
      }

      for (const auto &expr : raw_expressions) {
        results.push_back(EvaluateExpressionString(expr));
      }

      info.placeholders["${" + key + "}"] = results;
    }
  }
  return true;
}

// Executes an execute operation.
bool ExecuteExecute(const json &op, PlaceholderInfo &info,
                    std::map<std::string, bool> &third_party_files) {
  long long newest_input = -1;
  long long oldest_output = -2; // Sentinel for infinity concept
  bool missing_output = false;

  if (op.contains("inputs")) {
    auto inputs = EvaluatePath(JsonToStringVector(op["inputs"]), info);
    for (const auto &input : inputs) {
      if (!std::filesystem::exists(input)) {
        std::cerr << "Input does not exist: " << input << std::endl;
        return false;
      }
      long long ts = GetTimestampOfFile(input);
      if (ts > newest_input)
        newest_input = ts;
    }
  }

  std::vector<std::string> final_outputs;
  if (op.contains("outputs")) {
    auto outputs = EvaluatePath(JsonToStringVector(op["outputs"]), info);
    for (const auto &output : outputs) {
      final_outputs.push_back(output);
      third_party_files[output] = true;
      if (std::filesystem::exists(output)) {
        long long ts = GetTimestampOfFile(output);
        if (oldest_output == -2 || ts < oldest_output)
          oldest_output = ts;
      } else {
        missing_output = true;
        std::filesystem::create_directories(
            std::filesystem::path(output).parent_path());
      }
    }
  }

  bool always_run = op.value("alwaysRun", false);
  if (!missing_output && oldest_output != -2 && newest_input < oldest_output &&
      !always_run && newest_input != -1) {
    return true;
  }

  for (const auto &output : final_outputs) {
    if (std::filesystem::exists(output))
      std::filesystem::remove(output);
  }

  std::string command = op.value("command", "");
  auto cmds = SubstitutePlaceholdersInString(command, info);
  if (cmds.empty())
    return false;
  command = cmds[0];

  std::string cwd = "";
  if (op.contains("directory")) {
    auto dirs = SubstitutePlaceholdersInString(op.value("directory", ""), info);
    if (!dirs.empty())
      cwd = dirs[0];
  }

  std::string final_cmd =
      cwd.empty() ? command : "cd " + cwd + " && " + command;
  std::cout << "Executing: " << final_cmd << std::endl;
  if (!ExecuteSystemCommand(final_cmd))
    return false;

  return true;
}

// Executes a joinArray operation.
bool ExecuteJoinArray(const json &op, PlaceholderInfo &info,
                      std::map<std::string, bool> &third_party_files) {
  std::vector<std::string> raw_values;
  if (op["value"].is_string())
    raw_values.push_back(op["value"].get<std::string>());
  else if (op["value"].is_array())
    raw_values = JsonToStringVector(op["value"]);

  auto processed_values = SubstitutePlaceholders(raw_values, info);

  std::string joint = op.value("joint", "");
  auto joints = SubstitutePlaceholdersInString(joint, info);
  std::string joint_val = joints.empty() ? "" : joints[0];

  std::stringstream ss;
  for (size_t i = 0; i < processed_values.size(); i++) {
    if (i > 0)
      ss << joint_val;
    ss << processed_values[i];
  }

  std::string placeholder_name = op.value("placeholder", "");
  info.placeholders["${" + placeholder_name + "}"] = {ss.str()};

  return true;
}

// Executes a readFilesInDirectory operation.
bool ExecuteReadFilesInDirectory(
    const json &op, PlaceholderInfo &info,
    std::map<std::string, bool> &third_party_files) {
  auto paths = SubstitutePlaceholders(JsonToStringVector(op["path"]), info);

  std::vector<std::string> files_found;
  bool full_path = op.value("fullPath", false);

  std::set<std::string> extensions;
  if (op.contains("extensions")) {
    for (const auto &ext : op["extensions"])
      extensions.insert(ext.get<std::string>());
  }

  for (const auto &dir : paths) {
    if (!std::filesystem::exists(dir)) {
      std::cerr << "Directory does not exist: " << dir << std::endl;
      return false;
    }

    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_directory())
        continue;
      if (!extensions.empty() &&
          extensions.find(entry.path().extension().string()) ==
              extensions.end())
        continue;

      std::string val =
          full_path ? entry.path().string() : entry.path().filename().string();
      files_found.push_back(val);
    }
  }

  std::string placeholder_name = op.value("placeholder", "");
  info.placeholders["${" + placeholder_name + "}"] = files_found;
  return true;
}

// Executes a readRegExFromFile operation.
bool ExecuteReadRegExFromFile(const json &op, PlaceholderInfo &info,
                              std::map<std::string, bool> &third_party_files) {
  auto file_paths = EvaluatePath(JsonToStringVector(op["file"]), info);
  if (file_paths.empty())
    return false;
  std::string path = file_paths[0];

  if (!std::filesystem::exists(path)) {
    std::cerr << "File does not exist: " << path << std::endl;
    return false;
  }

  std::ifstream t(path);
  std::stringstream buffer;
  buffer << t.rdbuf();
  std::string content = buffer.str();

  if (op.contains("values")) {
    for (auto &[key_list, regex_str] : op["values"].items()) {
      std::regex re(regex_str.get<std::string>());
      std::smatch match;
      if (std::regex_search(content, match, re)) {
        std::stringstream ss(key_list);
        std::string segment;
        int idx = 0;
        while (std::getline(ss, segment, ',')) {
          if (idx < match.size()) {
            if (!segment.empty()) {
              info.placeholders["${" + segment + "}"] = {match[idx].str()};
            }
          }
          idx++;
        }
      }
    }
  }
  return true;
}

// Executes a set operation.
bool ExecuteSet(const json &op, PlaceholderInfo &info,
                std::map<std::string, bool> &third_party_files) {
  if (op.contains("values")) {
    for (const auto &[key, val] : op["values"].items()) {
      std::vector<std::string> results;
      if (val.is_string()) {
        results = SubstitutePlaceholdersInString(val.get<std::string>(), info);
      } else if (val.is_array()) {
        results = SubstitutePlaceholders(JsonToStringVector(val), info);
      }
      info.placeholders["${" + key + "}"] = results;
    }
  }
  return true;
}

// Dispatches to the appropriate operation executor.
bool ExecuteOperation(const json &op, PlaceholderInfo &info,
                      std::map<std::string, bool> &third_party_files) {
  static const std::map<std::string,
                        std::function<bool(const json &, PlaceholderInfo &,
                                           std::map<std::string, bool> &)>>
      operations = {
          {"copy", ExecuteCopy},
          {"createDirectory", ExecuteCreateDirectory},
          {"evaluate", ExecuteEvaluate},
          {"execute", ExecuteExecute},
          {"joinArray", ExecuteJoinArray},
          {"readFilesInDirectory", ExecuteReadFilesInDirectory},
          {"readRegExFromFile", ExecuteReadRegExFromFile},
          {"set", ExecuteSet},
      };

  std::string type = op.value("operation", "");
  auto it = operations.find(type);
  if (it != operations.end()) {
    return it->second(op, info, third_party_files);
  }

  std::cerr << "Unknown operation: " << type << std::endl;
  return false;
}

} // namespace

// Updates third party packages.
bool UpdateThirdParty(const std::filesystem::path &package_path) {
  std::filesystem::path third_party_json = package_path / "third_party.json";
  std::filesystem::path third_party_files_json =
      package_path / ".third_party_files.json";

  if (!std::filesystem::exists(third_party_json))
    return true; // Nothing to do

  // Check timestamps
  long long tp_time = GetTimestampOfFile(third_party_json.string());
  long long tpf_time = std::filesystem::exists(third_party_files_json)
                           ? GetTimestampOfFile(third_party_files_json.string())
                           : 0;

  if (tpf_time >= tp_time)
    return true; // Up to date

  std::cout << "Updating third party packages for "
            << GetPackageNameFromPath(package_path) << "..." << std::endl;

  std::ifstream f(third_party_json);
  json config;
  try {
    f >> config;
  } catch (const std::exception &e) {
    std::cerr << "Failed to parse " << third_party_json << ": " << e.what()
              << std::endl;
    return false;
  }

  PlaceholderInfo info;
  info.placeholders["${@}"] = {package_path.string()};

  LoadRepositoriesMap();

  if (config.contains("repositories")) {
    for (const auto &repo : config["repositories"]) {
      if (!LoadRepository(repo, info))
        return false;
    }
  }
  FlushRepositoriesMap();

  std::map<std::string, bool> third_party_files;

  if (config.contains("operations")) {
    for (const auto &op : config["operations"]) {
      if (!ExecuteOperation(op, info, third_party_files))
        return false;
    }
  }

  // Write .third_party_files.json
  json output_files = json::object();
  for (auto const &[file, val] : third_party_files) {
    output_files[file] = val;
  }
  std::ofstream out(third_party_files_json);
  out << output_files.dump(4);

  return true;
}

bool MaybeUpdateThirdPartyBeforeBuilding(
    const std::filesystem::path &package_path) {
  bool should_update = ShouldUpdateThirdParty();
  if (!should_update) {
    if (std::filesystem::exists(package_path / "third_party.json") &&
        !std::filesystem::exists(package_path / ".third_party_files.json")) {
      should_update = true;
    }
  }

  if (!should_update)
    return true;

  return UpdateThirdParty(package_path);
}

// Helper to update third party packages.
bool UpdateThirdPartyPackages() {
  bool success = true;
  ForEachInputPackage([&](const std::string &package_path_str) {
    success &= UpdateThirdParty(package_path_str);
  });
  return success;
}

bool CleanThirdParty(const std::filesystem::path &package_path) {
  std::filesystem::path third_party_files_json =
      package_path / ".third_party_files.json";
  if (!std::filesystem::exists(third_party_files_json))
    return true;

  try {
    std::ifstream f(third_party_files_json);
    json j;
    f >> j;
    for (auto &[path_str, val] : j.items()) {
      if (std::filesystem::exists(path_str)) {
        std::filesystem::remove(path_str);
      }
    }
    std::filesystem::remove(third_party_files_json);
  } catch (const std::exception &e) {
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
  return true;
}
