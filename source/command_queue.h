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

#include <memory>

#include "deferred_command.h"
#include "stage.h"

// Queues up a command for a stage.
void QueueCommand(Stage stage,
                  std::unique_ptr<DeferredCommand> deferred_command);

// Runs through the queued commands for each stage. Returns if they were all
// successful.
bool RunQueuedCommands();
