// posts.h — 文章正文首行 JSON 与分类映射（CLI / TTY 共用）
#pragma once
#include <string>
#include <vector>

namespace bn {

/* 主站分类 id ↔ 名称（与 assets/data/categories.json 一致） */
const char* catName(int id);
int catId(const std::string& s);

/* 正文首行 JSON（站点约定） */
struct PostMeta {
  std::string info;
  std::string icon;
  std::string coverText;
  int category = 0;
  bool hasCategory = false;
  bool tpl = false;
  bool allowComments = true;
  std::vector<std::string> tags;
  std::string rest;
};

PostMeta parsePostBody(const std::string& body);
std::string stripFirstLine(const std::string& body);

}  // namespace bn
