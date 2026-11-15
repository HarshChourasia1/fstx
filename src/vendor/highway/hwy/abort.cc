// Copyright 2019 Google LLC
// Copyright 2024 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0
// SPDX-License-Identifier: BSD-3-Clause

#include "hwy/abort.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <stdexcept>
#include <string>

#include "hwy/base.h"

namespace hwy {

namespace {

std::atomic<WarnFunc>& AtomicWarnFunc() {
  static std::atomic<WarnFunc> func;
  return func;
}

std::atomic<AbortFunc>& AtomicAbortFunc() {
  static std::atomic<AbortFunc> func;
  return func;
}

std::string GetBaseName(std::string const& file_name) {
  auto last_slash = file_name.find_last_of("/\\");
  return file_name.substr(last_slash + 1);
}

}  // namespace

// Returning a reference is unfortunately incompatible with `std::atomic`, which
// is required to safely implement `SetWarnFunc`. As a workaround, we store a
// copy here, update it when called, and return a reference to the copy. This
// has the added benefit of protecting the actual pointer from modification.
HWY_DLLEXPORT WarnFunc& GetWarnFunc() {
  static WarnFunc func;
  func = AtomicWarnFunc().load();
  return func;
}

HWY_DLLEXPORT AbortFunc& GetAbortFunc() {
  static AbortFunc func;
  func = AtomicAbortFunc().load();
  return func;
}

HWY_DLLEXPORT WarnFunc SetWarnFunc(WarnFunc func) {
  return AtomicWarnFunc().exchange(func);
}

HWY_DLLEXPORT AbortFunc SetAbortFunc(AbortFunc func) {
  return AtomicAbortFunc().exchange(func);
}

HWY_DLLEXPORT void HWY_FORMAT(3, 4)
    Warn(const char* file, int line, const char* format, ...) {
  char buf[800];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  WarnFunc handler = AtomicWarnFunc().load();
  if (handler != nullptr) {
    handler(file, line, buf);
  }
  // FST-X local change: no default print to stderr (not allowed in R packages); the warning is dropped.
}

HWY_DLLEXPORT HWY_NORETURN void HWY_FORMAT(3, 4)
    Abort(const char* file, int line, const char* format, ...) {
  char buf[800];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);

  AbortFunc handler = AtomicAbortFunc().load();
  if (handler != nullptr) {
    handler(file, line, buf);
  }
  // FST-X local change: instead of printing to stderr and calling abort() (which would end the R session),
  // throw; FST-X's R entry points turn C++ exceptions into R errors.
  throw std::runtime_error(std::string("Highway: ") + GetBaseName(file) + ":" + std::to_string(line) +
                           ": " + buf);
}

}  // namespace hwy
