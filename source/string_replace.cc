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

#include "string_replace.h"

// Replaces a placeholder in a string with a new value. Returns if the
// placeholder was found.
bool ReplaceSubstringInString(std::string& str, const std::string& placeholder,
                              const std::string& new_value) {
  size_t index =
      str.find(placeholder.c_str(), /*pos=*/0, /*n=*/placeholder.size());
  if (index == std::string::npos) return false;

  str.replace(index, placeholder.size(), new_value.c_str(), new_value.size());
  return true;
}
