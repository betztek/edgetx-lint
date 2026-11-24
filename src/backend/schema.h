// edgetx-lint: helpers over EdgeTX's compiled YAML schema (YamlNode tables)
#pragma once

#include <map>
#include <string>
#include <vector>

struct YamlNode;

// Index of every tag in a schema tree -> template paths where it is valid,
// e.g. "weight" -> {"mixData[].weight", "expoData[].weight"}.
class SchemaIndex
{
 public:
  void build(const YamlNode* root);
  std::vector<std::string> pathsFor(const std::string& tag) const;
  // All tags in the schema (for global did-you-mean)
  const std::multimap<std::string, std::string>& all() const { return idx_; }

 private:
  void walk(const YamlNode* node, const std::string& prefix, int depth);
  std::multimap<std::string, std::string> idx_;
};

// Tags directly addressable under `container` (descends into anonymous unions)
std::vector<std::string> childTags(const YamlNode* container);

// True if `container` is a dash list (array of >1 elements not keyed by IDX)
bool isDashList(const YamlNode* container);

// True if `container` is an array whose elements are keyed by an index key
bool isIdxArray(const YamlNode* container);

// Edit distance, case-insensitive
size_t editDistance(const std::string& a, const std::string& b);

// Best close matches of `word` among `candidates` (closest first, max n)
std::vector<std::string> closest(const std::string& word,
                                 const std::vector<std::string>& candidates,
                                 size_t n = 3);
