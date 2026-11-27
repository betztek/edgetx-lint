// edgetx-lint backend: one binary per (radio, EdgeTX version).
// Normally started by the `edgetx-lint` dispatcher, but usable directly.

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "findings.h"
#include "linter.h"
#include "report.h"
#include "describe.h"

static const char* kUsage =
    "usage: %s check [--radio R] [--edgetx TAG] [--format text|json|github]\n"
    "                [--warn-unknown] [--strict] [--quiet] [--type model|radio]\n"
    "                [--no-roundtrip] FILE...\n"
    "       %s describe [--multi Multi.txt] [--strict] [--compact] [RADIO/radio.yml] MODEL...\n"
    "       %s dump-default model|radio\n"
    "       %s roundtrip [--type model|radio] FILE\n"
    "       %s info\n";

// EdgeTX's simu code TRACEs to stdout, including from static constructors
// (widget/layout registration). Redirect fd 1 before any of that runs and keep
// the real stdout for our own output only. EDGETX_LINT_TRACE=1 sends EdgeTX's
// traces to stderr instead.
static int g_realOut = 1;

__attribute__((constructor(101))) static void earlyRedirect()
{
  fflush(stdout);
  g_realOut = dup(1);
  if (getenv("EDGETX_LINT_TRACE")) {
    dup2(2, 1);
  } else {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, 1);
      close(dn);
    }
  }
}

static std::string normTag(std::string t)
{
  if (!t.empty() && t[0] != 'v') t = "v" + t;
  return t;
}

int main(int argc, char** argv)
{
  FILE* out = fdopen(g_realOut, "w");
  if (!out) return 2;
  const char* self = argv[0];
  auto usage = [&]() {
    fprintf(stderr, kUsage, self, self, self, self, self);
    return 2;
  };
  if (argc < 2) return usage();

  std::string cmd = argv[1];
  Options opt;
  std::string radio, tag;
  std::vector<std::string> files;
  DescribeOptions dopt;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](std::string& dst) {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    if (a == "--radio") { if (!need(radio)) return usage(); }
    else if (a == "--edgetx") { if (!need(tag)) return usage(); }
    else if (a == "--format") { if (!need(opt.format)) return usage(); }
    else if (a == "--type") { if (!need(opt.forceType)) return usage(); }
    else if (a == "--warn-unknown") opt.warnUnknown = true;
    else if (a == "--strict") opt.strict = true;
    else if (a == "--quiet" || a == "-q") opt.quiet = true;
    else if (a == "--no-roundtrip") opt.noRoundtrip = true;
    else if (a == "--multi" && cmd == "describe") { if (!need(dopt.multiFile)) return usage(); }
    else if (a == "--compact" && cmd == "describe") dopt.pretty = false;
    else if (a == "--") { for (++i; i < argc; ++i) files.push_back(argv[i]); }
    else if (a.size() > 1 && a[0] == '-') {
      fprintf(stderr, "edgetx-lint: unknown option '%s'\n", a.c_str());
      return 2;
    } else files.push_back(a);
  }

  if (cmd == "info" || cmd == "--version") {
    fprintf(out, "edgetx-lint %s backend: radio=%s edgetx=%s\n", LINT_VERSION, LINT_RADIO,
            LINT_EDGETX_TAG);
    fflush(out);
    return 0;
  }

  if (opt.format != "text" && opt.format != "json" && opt.format != "github") {
    fprintf(stderr, "edgetx-lint: --format must be text, json or github\n");
    return 2;
  }
  if (!opt.forceType.empty() && opt.forceType != "model" && opt.forceType != "radio") {
    fprintf(stderr, "edgetx-lint: --type must be model or radio\n");
    return 2;
  }
  if (!radio.empty() && radio != LINT_RADIO) {
    fprintf(stderr, "edgetx-lint: this backend is for radio '%s', not '%s'\n", LINT_RADIO,
            radio.c_str());
    return 2;
  }
  if (!tag.empty() && normTag(tag) != LINT_EDGETX_TAG) {
    fprintf(stderr, "edgetx-lint: this backend is for EdgeTX %s, not %s\n", LINT_EDGETX_TAG,
            tag.c_str());
    return 2;
  }

  backendInit();

  if (cmd == "dump-default") {
    if (files.size() != 1) return usage();
    std::string text;
    int rc = dumpDefault(files[0], text);
    fwrite(text.data(), 1, text.size(), out);
    fflush(out);
    return rc;
  }
  if (cmd == "roundtrip") {
    if (files.size() != 1) return usage();
    std::string text;
    int rc = roundtripDump(files[0], opt.forceType, text);
    fwrite(text.data(), 1, text.size(), out);
    fflush(out);
    return rc;
  }
  if (cmd == "describe") {
    if (files.empty()) {
      fprintf(stderr, "edgetx-lint: no files given\n");
      return 2;
    }
    dopt.strict = opt.strict;
    int rc = describeFiles(out, files, dopt);
    fflush(out);
    return rc;
  }
  if (cmd != "check") return usage();
  if (files.empty()) {
    fprintf(stderr, "edgetx-lint: no files given\n");
    return 2;
  }

  std::vector<FileResult> results;
  for (auto& f : files) results.push_back(lintFile(f, opt));

  RunInfo info{LINT_RADIO, LINT_EDGETX_TAG, LINT_VERSION};
  int rc = report(out, results, opt, info);
  fflush(out);
  return rc;
}
