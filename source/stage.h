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

#include <string_view>

// A build stage. Deferred commands are added to the command queue grouped by
// stage. The stages are in order. Commands from earlier stages run before
// commands from later stages. Commands in the same stage can be ran out of
// order and in parallel.
enum class Stage {
  // When the individual source files are compiled.
  Compile = 0,
  // When the libraries are linked.
  LinkLibrary = 1,
  // When the build source files and libraries are linked together into
  // applications.
  LinkApplication = 2,
  // When the applications run.
  Run = 3
};
