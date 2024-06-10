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

#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {

// A map of placeholders to their replacement values.
std::map<std::string, std::string> placeholders_and_values;

} // namespace

// Registers a placeholder for use with `ReplacePlaceholdersInString`. The
// placeholder excludes the "${}". e.g. "${abc}" will be just "abc".
void SetPlaceholder(std::string placeholder, std::string_view str) {
  placeholders_and_values[placeholder] = std::string(str);
}

// Replaces all registered placeholders in a string with a new value.
void ReplacePlaceholdersInString(std::string& str) {
  size_t pos = 0; // Current position in the string.
    
  while ((pos = str.find("${", pos)) != std::string::npos) {
      size_t end_pos = str.find("}", pos + 2); // Find closing "}"

      if (end_pos != std::string::npos) {
          // Get the placeholder name.
          std::string placeholder = str.substr(pos + 2, end_pos - pos - 2);
          // Look up replacement value.
          const auto& it = placeholders_and_values.find(placeholder);

          if (it != placeholders_and_values.end()) {
              // Replace placeholder with the replacement value.
              str.replace(pos, end_pos - pos + 1, it->second);
               // Move past the replacement.
              pos += it->second.length();
          } else {
              // Replace the placeholder with an empty string.
              std::cerr << "Encountered unknown placeholder: ${" << placeholder << "}" << std::endl;
              str.replace(pos, end_pos - pos + 1, "");
          }
      } else {
          break; // Invalid placeholder format
      }
  }
}

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
