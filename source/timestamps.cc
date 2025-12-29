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

#include "timestamps.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>

namespace {

// A cache of timestamps of files.
std::map<std::string, uint64_t> timestamps_by_filename;

std::string NormalizePath(const std::string &path) {
  try {
    return std::filesystem::weakly_canonical(path).string();
  } catch (...) {
    return path;
  }
}

}  // namespace

uint64_t GetTimestampOfFile(const std::string& file_name) {
  std::string normalized_name = NormalizePath(file_name);
  auto itr = timestamps_by_filename.find(normalized_name);
  if (itr != timestamps_by_filename.end()) {
    return itr->second;
  }

  uint64_t timestamp = 0;
  if (std::filesystem::exists(normalized_name)) {
    std::error_code ec;
    auto last_write_time =
        std::filesystem::last_write_time(normalized_name, ec);
    if (!ec) {  // Only proceed if last_write_time was successful.
      auto time_since_epoch = last_write_time.time_since_epoch();
      timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                      time_since_epoch)
                      .count();
    }
  }
  timestamps_by_filename[normalized_name] = timestamp;
  return timestamp;
}

bool DoesFileExist(const std::string& file_name) {
  return GetTimestampOfFile(file_name) != 0;
}

void SetTimestampOfFileToNow(const std::string& file_name) {
  std::string normalized_name = NormalizePath(file_name);
  auto now = std::chrono::system_clock::now();
  auto time_since_epoch = now.time_since_epoch();
  timestamps_by_filename[normalized_name] =
      std::chrono::duration_cast<std::chrono::milliseconds>(time_since_epoch)
          .count();
}

void InvalidateTimestamp(const std::string &file_name) {
  std::string normalized_name = NormalizePath(file_name);
  timestamps_by_filename.erase(normalized_name);
}
