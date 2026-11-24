// edgetx-lint: core engine (runs EdgeTX's own YAML parser and writer)
#pragma once

#include <string>

#include "findings.h"

// Lint one file. Never throws; problems are reported as findings.
FileResult lintFile(const std::string& path, const Options& opt);

// Emit EdgeTX's default model / radio settings as written by this build
// (used to generate synthetic, known-good fixtures).
int dumpDefault(const std::string& what, std::string& out);

// Re-emit a file exactly as this EdgeTX build would save it after loading.
int roundtripDump(const std::string& path, const std::string& forceType,
                  std::string& out);

void backendInit();
