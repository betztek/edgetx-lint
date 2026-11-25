// edgetx-lint: output formatting (text, json, github annotations)

#include "report.h"

#include <string>

const char* severityName(Severity s)
{
  switch (s) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    default: return "info";
  }
}

static std::string jsonStr(const std::string& s)
{
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else
          o += (char)c;
    }
  }
  return o + "\"";
}

// GitHub workflow command escaping
static std::string ghData(const std::string& s)
{
  std::string o;
  for (char c : s) {
    if (c == '%') o += "%25";
    else if (c == '\r') o += "%0D";
    else if (c == '\n') o += "%0A";
    else o += c;
  }
  return o;
}

static std::string ghProp(const std::string& s)
{
  std::string o;
  for (char c : ghData(s)) {
    if (c == ':') o += "%3A";
    else if (c == ',') o += "%2C";
    else o += c;
  }
  return o;
}

static bool shown(const Finding& f, const Options& opt)
{
  return !(opt.quiet && f.sev == Severity::Info);
}

int report(FILE* out, const std::vector<FileResult>& results, const Options& opt,
           const RunInfo& info)
{
  int errors = 0, warnings = 0, infos = 0;
  for (auto& r : results)
    for (auto& f : r.findings) {
      if (f.sev == Severity::Error) errors++;
      else if (f.sev == Severity::Warning) warnings++;
      else infos++;
    }

  if (opt.format == "json") {
    fprintf(out, "{\n  \"tool\": \"edgetx-lint\",\n  \"version\": %s,\n",
            jsonStr(info.version).c_str());
    fprintf(out, "  \"radio\": %s,\n  \"edgetx\": %s,\n", jsonStr(info.radio).c_str(),
            jsonStr(info.edgetx).c_str());
    fprintf(out, "  \"files\": [");
    bool firstFile = true;
    for (auto& r : results) {
      fprintf(out, "%s\n    {\n      \"file\": %s,\n      \"kind\": %s,\n", firstFile ? "" : ",",
              jsonStr(r.file).c_str(), jsonStr(r.kind).c_str());
      firstFile = false;
      fprintf(out, "      \"semver\": %s,\n      \"loaded\": %s,\n      \"findings\": [",
              r.semver.empty() ? "null" : jsonStr(r.semver).c_str(), r.loadOk ? "true" : "false");
      bool first = true;
      for (auto& f : r.findings) {
        if (!shown(f, opt)) continue;
        fprintf(out,
                "%s\n        {\"severity\": %s, \"rule\": %s, \"line\": %d, \"path\": %s, "
                "\"message\": %s, \"hint\": %s}",
                first ? "" : ",", jsonStr(severityName(f.sev)).c_str(), jsonStr(f.rule).c_str(),
                f.line, jsonStr(f.path).c_str(), jsonStr(f.message).c_str(),
                f.hint.empty() ? "null" : jsonStr(f.hint).c_str());
        first = false;
      }
      fprintf(out, "%s]\n    }", first ? "" : "\n      ");
    }
    fprintf(out, "\n  ],\n  \"summary\": {\"files\": %zu, \"errors\": %d, \"warnings\": %d, \"info\": %d}\n}\n",
            results.size(), errors, warnings, infos);
  } else if (opt.format == "github") {
    for (auto& r : results)
      for (auto& f : r.findings) {
        if (!shown(f, opt)) continue;
        const char* cmd = f.sev == Severity::Error ? "error"
                          : f.sev == Severity::Warning ? "warning" : "notice";
        std::string props = "file=" + ghProp(r.file);
        if (f.line > 0) props += ",line=" + std::to_string(f.line);
        props += ",title=" + ghProp("edgetx-lint " + f.rule);
        std::string msg = (f.path.empty() ? "" : f.path + ": ") + f.message;
        if (!f.hint.empty()) msg += " (" + f.hint + ")";
        fprintf(out, "::%s %s::%s\n", cmd, props.c_str(), ghData(msg).c_str());
      }
    fprintf(out, "edgetx-lint %s (%s, EdgeTX %s): %zu file(s), %d error(s), %d warning(s)\n",
            info.version.c_str(), info.radio.c_str(), info.edgetx.c_str(), results.size(),
            errors, warnings);
  } else {
    for (auto& r : results)
      for (auto& f : r.findings) {
        if (!shown(f, opt)) continue;
        fprintf(out, "%s:%d: %s [%s] %s%s\n", r.file.c_str(), f.line, severityName(f.sev),
                f.rule.c_str(), f.path.empty() ? "" : (f.path + ": ").c_str(), f.message.c_str());
        if (!f.hint.empty()) fprintf(out, "    hint: %s\n", f.hint.c_str());
      }
    fprintf(out, "%zu file(s) checked (%s, EdgeTX %s): %d error(s), %d warning(s), %d info\n",
            results.size(), info.radio.c_str(), info.edgetx.c_str(), errors, warnings, infos);
  }

  if (errors) return 1;
  if (opt.strict && warnings) return 1;
  return 0;
}
