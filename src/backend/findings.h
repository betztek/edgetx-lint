// edgetx-lint: shared result types (no EdgeTX headers here)
#pragma once

#include <string>
#include <vector>

enum class Severity { Error, Warning, Info };

const char* severityName(Severity s);

struct Finding {
  Severity sev = Severity::Error;
  std::string rule;     // stable rule id, e.g. "unknown-key"
  int line = 0;         // 1-based line in the input file, 0 = whole file
  std::string path;     // key path, e.g. "mixData[2].weight"
  std::string message;  // what is wrong
  std::string hint;     // optional: how to fix / did you mean
};

struct FileResult {
  std::string file;
  std::string kind;    // "model" | "radio" | "skipped"
  std::string semver;  // semver found in the file ("" if none)
  bool loadOk = true;  // EdgeTX's own loader accepted the file
  std::vector<Finding> findings;
};

struct Options {
  std::string format = "text";  // text | json | github
  std::string forceType;        // "" | model | radio
  bool warnUnknown = false;     // unknown-key errors -> warnings
  bool strict = false;          // warnings also fail (exit 1)
  bool quiet = false;           // hide info findings
  bool noRoundtrip = false;     // skip tier 2
};
