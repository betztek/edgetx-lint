// edgetx-lint: `describe` - facts JSON about what the radio loads
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "findings.h"

struct DescribeOptions {
  std::string multiFile;  // optional SD card Multi.txt (protocol names)
  bool strict = false;    // warnings also give exit 1
  bool pretty = true;     // indented JSON
};

// Describe every file (radio.yml is loaded first, whatever its position).
// Writes one JSON document to `out`. Returns the exit code:
// 0 clean, 1 lint errors in an input (or warnings with --strict), 2 usage.
int describeFiles(FILE* out, const std::vector<std::string>& files,
                  const DescribeOptions& opt);
