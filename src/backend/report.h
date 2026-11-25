// edgetx-lint: output formatting
#pragma once

#include <cstdio>
#include <vector>

#include "findings.h"

struct RunInfo {
  std::string radio;
  std::string edgetx;  // tag, e.g. v2.12.5
  std::string version; // edgetx-lint version
};

// Applies severity filtering (quiet) and prints in opt.format.
// Returns process exit code (0 clean, 1 findings).
int report(FILE* out, const std::vector<FileResult>& results, const Options& opt,
           const RunInfo& info);
