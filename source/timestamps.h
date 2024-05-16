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

#include <cstdint>
#include <string>

// Returns the timestamp of a file, or 0 if the file does not exist. The units
// do not matter, only that a more recent file has a higher number.
uint64_t GetTimestampOfFile(const std::string& file_name);

// Returns whether a file exists.
bool DoesFileExist(const std::string& file_name);

// Sets the timestamp of a file to now.
void SetTimestampOfFileToNow(const std::string& file_name);
