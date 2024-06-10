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

#include <string>
#include <string_view>

// Registers a placeholder for use with `ReplacePlaceholdersInString`. The
// placeholder excludes the "${}". e.g. "${abc}" will be just "abc".
void SetPlaceholder(std::string placeholder, std::string_view str);

// Replaces all registered placeholders in a string with a new value.
void ReplacePlaceholdersInString(std::string& str);

// Replaces a placeholder in a string with a new value. Returns if the
// placeholder was found. The placeholder is in full format, e.g. "${abc}"
bool ReplaceSubstringInString(std::string& str, const std::string& placeholder,
                              const std::string& new_value);
