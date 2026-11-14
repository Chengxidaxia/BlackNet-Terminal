#include "posts.h"

#include <exception>

#include "third_party/json.hpp"
#include "util.h"

namespace bn {

const char* catName(int id) {
  switch (id) {
    case 1: return "公告";
    case 2: return "新闻";
    case 3: return "资讯";
    case 4: return "技术";
    case 5: return "活动";
    case 6: return "随笔";
    default: return "";
  }
}

int catId(const std::string& s) {
  if (s.empty()) return -1;
  bool digits = true;
  for (char ch : s)
    if (ch < '0' || ch > '9') { digits = false; break; }
  if (digits) {
    try { return std::stoi(s); } catch (const std::exception&) { return -1; }
  }
  for (int i = 1; i <= 6; i++)
    if (s == catName(i)) return i;
  return -1;
}

PostMeta parsePostBody(const std::string& body) {
  PostMeta m;
  m.rest = body;
  size_t nl = body.find('\n');
  std::string head = trim(nl == std::string::npos ? body : body.substr(0, nl));
  if (head.empty() || head[0] != '{') return m;
  try {
    auto j = nlohmann::json::parse(head);
    if (j.contains("info") && j["info"].is_string()) {
      auto raw = b64decode(j["info"].get<std::string>());
      m.info = std::string(raw.begin(), raw.end());
    }
    if (j.contains("icon") && j["icon"].is_string() && !j["icon"].get<std::string>().empty()) {
      auto raw = b64decode(j["icon"].get<std::string>());
      m.icon = std::string(raw.begin(), raw.end());
    }
    if (j.contains("coverText") && j["coverText"].is_string())
      m.coverText = j["coverText"].get<std::string>();
    if (j.contains("category") && j["category"].is_number_integer()) {
      m.category = j["category"].get<int>();
      m.hasCategory = true;
    }
    if (j.contains("tpl") && j["tpl"].is_boolean()) m.tpl = j["tpl"].get<bool>();
    if (j.contains("allowComments") && j["allowComments"].is_boolean())
      m.allowComments = j["allowComments"].get<bool>();
    if (j.contains("tags") && j["tags"].is_array())
      for (auto& t : j["tags"])
        if (t.is_string()) m.tags.push_back(t.get<std::string>());
    m.rest = nl == std::string::npos ? "" : body.substr(nl + 1);
  } catch (const std::exception&) {
  }
  return m;
}

std::string stripFirstLine(const std::string& body) {
  size_t nl = body.find('\n');
  return nl == std::string::npos ? body : body.substr(nl + 1);
}

}  // namespace bn
