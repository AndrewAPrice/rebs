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

// The action that REBS was invocated to do.
enum class InvocationAction {
  // Deep-clean everything.
  DeepClean,
  // Clean all temporary files associated with provided packages. This will
  // force a complete rebuild of the packages when they are re-ran.
  Clean,
  // Builds the provided packages.
  Build,
  // Builds and runs the provided packages.
  Run,
  // Builds and runs unit tests for the provided packages.
  Test,
  // Lists all packages with their names and paths.
  List
};
