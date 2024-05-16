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

// The level of optimization to enable when building packages.
enum class OptimizationLevel {
  // All debug symbols.
  Debug,
  // Default level of optimization, for building really quickly.
  Fast,
  // Aggressive, whole program optimization.
  Optimized
};

// Converts an optimization level into a human readable string.
std::string_view OptimizationLevelToString(OptimizationLevel level);
