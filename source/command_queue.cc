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

#include "command_queue.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "deferred_command.h"
#include "dependencies.h"
#include "execute.h"
#include "invocation.h"
#include "stage.h"
#include "string_replace.h"
#include "temp_directory.h"
#include "terminal.h"

namespace {

// The prefix of the name of the file that is temporarily used by compilers to
// write the dependencies to.
constexpr char kDependencyFilePrefix[] = "deps";

// A list of commands, grouped by the stage the command is for.
std::map<int, std::vector<std::unique_ptr<DeferredCommand>>>
    deferred_commands_by_stage;

// The highest encountered stage, for which there is a queued command.
int max_stage = 0;

//
int queued_commands_count = 0;
bool needs_newline = true;

// Run the commands sequentually, with no piping of the input/output.
void RunCommands(const std::vector<std::unique_ptr<DeferredCommand>>& commands,
                 int& current_command_number) {
  if (needs_newline) {
    std::cout << kEraseLine << std::flush;
    needs_newline = false;
  }

  bool should_be_verbose = ShouldBeVerbose();

  for (const auto& command : commands) {
    if (should_be_verbose) {
      std::cout << kEraseLine << "Running " << current_command_number++ << "/"
                << queued_commands_count << ": " << command->command
                << std::endl;
    }
    std::system(command->command.c_str());
  }
}

bool ExecuteStage(Stage stage,
                  const std::vector<std::unique_ptr<DeferredCommand>>& commands,
                  int& current_command_number,
                  std::stringstream& combined_output) {
  if (stage == Stage::Run || ShouldBeVerbose()) {
    RunCommands(commands, current_command_number);
    return true;
  }

  bool record_dependencies = stage == Stage::Compile;
  needs_newline = true;
  // std::string_view verb = StageToVerb(static_cast<Stage>(stage));
  int total_commands = commands.size();
  int next_command_index = 0;

  std::mutex mutex;
  bool successful = true;

  std::mutex dependencies_mutex;

  // Returns the next command. Thread safe.
  auto get_next_deferred_command =
      [&mutex, &next_command_index, &commands, &total_commands,
       &current_command_number]() -> DeferredCommand* {
    std::scoped_lock lock(mutex);
    if (next_command_index >= total_commands) return nullptr;
    auto command = commands[next_command_index++].get();

    std::cout << kEraseLine << "Running " << current_command_number++ << "/"
              << queued_commands_count << std::flush;
    return command;
  };

  // Create each thread. There's no point creating more threads than the number
  // of commands to run.
  int thread_count = std::min(total_commands, GetNumberOfParallelTasks());
  std::vector<std::thread> threads;
  for (int thread_no = 0; thread_no < thread_count; thread_no++) {
    threads.push_back(std::thread([&get_next_deferred_command, &successful,
                                   &mutex, &combined_output,
                                   &dependencies_mutex, record_dependencies,
                                   thread_no]() {
      bool thread_successful = true;
      std::stringstream output;

      std::string dependency_file;
      std::string quoted_dependency_file;
      if (record_dependencies) {
        dependency_file = GetTempDependencyFilePath(thread_no);
        quoted_dependency_file =
            (std::stringstream() << std::quoted(dependency_file.c_str())).str();
      }

      while (auto* command = get_next_deferred_command()) {
        if (record_dependencies) {
          std::string command_str = command->command;
          bool using_dependency_file = ReplaceSubstringInString(
              command_str, "${deps file}", quoted_dependency_file);
          if (ExecuteCommand(command_str, &output)) {
            std::vector<std::filesystem::path> dependencies;
            if (using_dependency_file) {
              dependencies = ReadDependenciesFromFile(dependency_file);
            } else {
              dependencies = {command->source_file};
            }
            std::scoped_lock lock(dependencies_mutex);
            SetDependenciesOfFile(command->package_id,
                                  command->destination_file, dependencies);
          } else {
            thread_successful = false;
          }
        } else {
          // Simplified path where the command does not need to be copied.
          if (!ExecuteCommand(command->command, &output)) {
            thread_successful = false;
          }
        }
      }

      if (!thread_successful) {
        std::scoped_lock lock(mutex);
        combined_output << output.rdbuf();
        successful = false;
      }
    }));
  }

  // Wait for all of the threads to finish running.
  for (auto& thread : threads) thread.join();

  return successful;
}

}  // namespace

void QueueCommand(Stage stage,
                  std::unique_ptr<DeferredCommand> deferred_command) {
  int stage_i = static_cast<int>(stage);
  auto itr = deferred_commands_by_stage.find(stage_i);

  if (itr == deferred_commands_by_stage.end()) {
    const auto [new_itr, _] = deferred_commands_by_stage.insert(
        {stage_i, std::vector<std::unique_ptr<DeferredCommand>>{}});
    itr = new_itr;
    max_stage = std::max(max_stage, stage_i);
  }
  itr->second.emplace_back(std::move(deferred_command));
  queued_commands_count++;
}

bool RunQueuedCommands() {
  int current_command_number = 1;
  bool successful = true;
  std::stringstream output;
  for (int stage = 0; stage <= max_stage; stage++) {
    auto itr = deferred_commands_by_stage.find(stage);
    if (itr == deferred_commands_by_stage.end()) continue;
    if (!ExecuteStage(static_cast<Stage>(stage), itr->second,
                      current_command_number, output)) {
      successful = false;
      break;
    }
  }

  if (needs_newline) {
    std::cout << std::endl;
    needs_newline = false;
  }

  if (!successful) {
    std::cerr << output.rdbuf();
    return false;
  }

  return true;
}
