// edgetx-lint: helpers over EdgeTX's compiled YAML schema (YamlNode tables)

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

#include "schema.h"
#include "etx.h"

static bool isAnonUnion(const YamlNode* n)
{
  return n->type == YDT_UNION && (!n->tag || n->tag[0] == '\0');
}

bool isIdxArray(const YamlNode* c)
{
  return c && c->type == YDT_ARRAY && c->u._array.child &&
         c->u._array.child[0].type == YDT_IDX;
}

bool isDashList(const YamlNode* c)
{
  return c && c->type == YDT_ARRAY && c->elmts > 1 && !isIdxArray(c);
}

static void collectTags(const YamlNode* c, std::vector<std::string>& out,
                        int depth)
{
  if (!c || depth > 4) return;
  if (c->type != YDT_ARRAY && c->type != YDT_UNION) return;
  for (const YamlNode* n = c->u._array.child; n && n->type != YDT_NONE; ++n) {
    if (n->type == YDT_PADDING || n->type == YDT_IDX) continue;
    if (isAnonUnion(n)) {
      collectTags(n, out, depth + 1);
      continue;
    }
    if (n->tag) out.emplace_back(n->tag);
  }
}

std::vector<std::string> childTags(const YamlNode* container)
{
  std::vector<std::string> out;
  collectTags(container, out, 0);
  return out;
}

void SchemaIndex::build(const YamlNode* root)
{
  idx_.clear();
  walk(root, "", 0);
}

void SchemaIndex::walk(const YamlNode* node, const std::string& prefix,
                       int depth)
{
  if (!node || depth > 12) return;
  for (const YamlNode* n = node->u._array.child; n && n->type != YDT_NONE;
       ++n) {
    if (n->type == YDT_PADDING || n->type == YDT_IDX) continue;
    if (isAnonUnion(n)) {
      walk(n, prefix, depth + 1);
      continue;
    }
    std::string p = prefix.empty() ? n->tag : prefix + "." + n->tag;
    idx_.emplace(n->tag, p);
    if (n->type == YDT_ARRAY) {
      std::string suffix = isIdxArray(n) ? "[N]" : (n->elmts > 1 ? "[]" : "");
      walk(n, p + suffix, depth + 1);
    } else if (n->type == YDT_UNION) {
      walk(n, p, depth + 1);
    }
  }
}

std::vector<std::string> SchemaIndex::pathsFor(const std::string& tag) const
{
  std::vector<std::string> out;
  auto r = idx_.equal_range(tag);
  for (auto it = r.first; it != r.second; ++it) out.push_back(it->second);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

size_t editDistance(const std::string& a, const std::string& b)
{
  const size_t n = a.size(), m = b.size();
  std::vector<size_t> prev(m + 1), cur(m + 1);
  for (size_t j = 0; j <= m; ++j) prev[j] = j;
  for (size_t i = 1; i <= n; ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= m; ++j) {
      bool same = std::tolower((unsigned char)a[i - 1]) ==
                  std::tolower((unsigned char)b[j - 1]);
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (same ? 0 : 1)});
      // transposition
      if (i > 1 && j > 1 &&
          std::tolower((unsigned char)a[i - 1]) == std::tolower((unsigned char)b[j - 2]) &&
          std::tolower((unsigned char)a[i - 2]) == std::tolower((unsigned char)b[j - 1]))
        cur[j] = std::min(cur[j], prev[j - 1]);  // approx. Damerau
    }
    std::swap(prev, cur);
  }
  return prev[m];
}

std::vector<std::string> closest(const std::string& word,
                                 const std::vector<std::string>& candidates,
                                 size_t n)
{
  std::vector<std::pair<size_t, std::string>> scored;
  std::set<std::string> seen;
  size_t limit = std::max<size_t>(2, word.size() / 3);
  for (auto& c : candidates) {
    if (c.empty() || !seen.insert(c).second) continue;
    size_t d = editDistance(word, c);
    // case-only difference counts as a strong match
    bool caseOnly = word.size() == c.size() &&
                    std::equal(word.begin(), word.end(), c.begin(),
                               [](char x, char y) {
                                 return std::tolower((unsigned char)x) ==
                                        std::tolower((unsigned char)y);
                               });
    if (caseOnly) d = 0;
    if (d <= limit) scored.emplace_back(d, c);
  }
  std::stable_sort(scored.begin(), scored.end(),
                   [](auto& x, auto& y) { return x.first < y.first; });
  std::vector<std::string> out;
  for (auto& s : scored) {
    if (out.size() >= n) break;
    out.push_back(s.second);
  }
  return out;
}
