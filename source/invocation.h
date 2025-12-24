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

#include <functional>
#include <string>

#include "invocation_action.h"
#include "optimization_level.h"

// Parses the invocation from the program's arguments.
bool ParseInvocation(int argc, char* argv[]);

// Returns the invocation action.
InvocationAction GetInvocationAction();

// Returns the optimization level to build with.
OptimizationLevel GetOptimizationLevel();

// Loops over each raw package that was used as the program's arguments. Even if
// none were provided, then this will call `on_each_package` once with a blank
// string.
void ForEachRawInputPackage(
    const std::function<void(const std::string&)>& on_each_package);

// Returns whether the user requested that the invocation action be performed
// for all known packages.
bool RunOnAllKnownPackages();

// Returns whether the user requested that third party packages be updated.
bool ShouldUpdateThirdParty();

// Returns whether REBS should be verbose about the running commands.
bool ShouldBeVerbose();

// Returns a list of all known flags that REBS accepts.
const std::vector<std::string> &GetKnownFlags();

// Returns the target string for completion (only valid if InvocationAction is
// Complete).
const std::string &GetCompletionTarget();
