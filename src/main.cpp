// main.cpp — bn（BlackNet CLI）命令入口
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "api.h"
#include "bookfile.h"
#include "config.h"
#include "http.h"
#include "posts.h"
#include "statusview.h"
#include "tty.h"
#include "version.h"
#include "third_party/json.hpp"
#include "ui.h"
#include "util.h"

namespace bn {
namespace {

/* ================= 参数解析 ================= */
struct Args {
  std::map<std::string, std::string> val;
  std::map<std::string, std::string> flag;
  std::vector<std::string> pos;

  bool has(const std::string& k) const {
    return val.count(k) || flag.count(k);
  }
  std::string str(const std::string& k, const std::string& def = "") const {
    auto it = val.find(k);
    return it == val.end() ? def : it->second;
  }
  long num(const std::string& k, long def) const {
    auto it = val.find(k);
    if (it == val.end()) return def;
    try { return std::stol(it->second); } catch (...) { return def; }
  }
};

const std::set<std::string> kBoolFlags = {"json", "no-color", "dry", "remove", "raw", "verbose",
                                          "yes", "help", "token-stdin", "password-stdin", "plain",
                                          "all", "short"};

Args parseArgs(int argc, char** argv) {
  Args a;
  for (int i = 0; i < argc; i++) {
    std::string s = argv[i];
    if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
      std::string name = s.substr(2);
      size_t eq = name.find('=');
      if (eq != std::string::npos) {
        a.val[name.substr(0, eq)] = name.substr(eq + 1);
      } else if (kBoolFlags.count(name)) {
        a.flag[name] = "1";
      } else if (i + 1 < argc && argv[i + 1][0] != '-') {
        a.val[name] = argv[++i];
      } else {
        a.flag[name] = "1";
      }
    } else if (s.size() == 2 && s[0] == '-') {
      std::string k(1, s[1]);
      static const std::map<std::string, std::string> alias = {
          {"n", "n"},   {"f", "file"}, {"t", "title"}, {"m", "message"}, {"o", "out"},
          {"c", "cat"}, {"q", "q"},    {"y", "yes"},   {"v", "verbose"},  {"h", "help"}};
      auto it = alias.find(k);
      if (it != alias.end()) {
        if (kBoolFlags.count(it->second)) a.flag[it->second] = "1";
        else if (i + 1 < argc) a.val[it->second] = argv[++i];
      }
    } else {
      a.pos.push_back(s);
    }
  }
  return a;
}

/* ================= 小工具 ================= */
std::string readStdinAll() {
  std::ostringstream ss;
  ss << std::cin.rdbuf();
  return ss.str();
}

std::string promptHidden(const std::string& label) {
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  bool restore = GetConsoleMode(h, &mode) != 0;
  if (restore) SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
  std::cout << label << std::flush;
  std::string line;
  std::getline(std::cin, line);
  if (restore) SetConsoleMode(h, mode);
  std::cout << "\n";
  return trim(line);
}

bool confirm(const std::string& q, bool assumeYes) {
  if (assumeYes) return true;
  std::cout << q << " [y/N] " << std::flush;
  std::string line;
  std::getline(std::cin, line);
  std::string t = lowerAscii(trim(line));
  return t == "y" || t == "yes";
}

Ctx makeCtx() {
  Ctx c;
  c.cfg = loadConfig();
  c.token = resolveToken(c.cfg);
  c.http.verbose = g.verbose;
  return c;
}

int errCode(const std::string& err) {
  if (err.find("401") != std::string::npos || err.find("403") != std::string::npos ||
      err.find("未登录") != std::string::npos || err.find("登录已过期") != std::string::npos ||
      err.find("身份") != std::string::npos)
    return EX_AUTH;
  if (err.find("网络错误") != std::string::npos || err.find("取不到") != std::string::npos)
    return EX_NET;
  if (err.find("404") != std::string::npos || err.find("不存在") != std::string::npos)
    return EX_NOTFOUND;
  return EX_FAIL;
}

bool requireToken(const Ctx& c) {
  if (!c.token.empty()) return true;
  fail(EX_AUTH, "需要登录：bn login（或设置 BLACKNET_TOKEN 环境变量）", "auth");
  return false;
}

/* 递归建目录 */
void MakeSureDir(const std::string& path) {
  std::wstring w = utf8ToWide(path);
  for (size_t i = 1; i <= w.size(); i++) {
    if (i == w.size() || w[i] == L'\\' || w[i] == L'/') {
      std::wstring sub = w.substr(0, i);
      CreateDirectoryW(sub.c_str(), nullptr);
    }
  }
}


/* ================= 登录 ================= */
int cmdLogin(const Args& a) {
  Ctx c = makeCtx();
  std::string token;
  if (a.has("token")) token = trim(a.str("token"));
  else if (a.has("token-stdin")) token = trim(readStdinAll());
  if (token.empty()) {
    if (!c.token.empty()) {
      std::cout << cYellow("已有一个登录态（" + c.cfg.login + "），重新登录会覆盖它。") << "\n";
    }
    std::cout << "粘贴 GitHub 令牌（需要 repo 或 public_repo 权限；输入不显示）：\n";
    token = promptHidden("token> ");
  }
  if (token.empty()) return fail(EX_USAGE, "没有拿到令牌");

  HttpResponse r = c.http.get("https://api.github.com/user",
                              {{"User-Agent", "BlackNet-CLI/0.1"},
                               {"Authorization", "Bearer " + token}});
  if (!r.error.empty()) return fail(EX_NET, "网络错误：" + r.error);
  nlohmann::json u;
  try {
    u = nlohmann::json::parse(r.body);
  } catch (const std::exception&) {
    return fail(EX_FAIL, "GitHub 返回非 JSON（HTTP " + std::to_string(r.status) + "）");
  }
  if (r.status != 200 || !u.contains("login"))
    return fail(EX_AUTH, "令牌无效或已过期（HTTP " + std::to_string(r.status) + "）", "auth");

  c.cfg.token = token;
  c.cfg.login = u.value("login", "");
  c.cfg.name = u.value("name", "");
  c.cfg.avatar = u.value("avatar_url", "");
  if (!saveConfig(c.cfg)) return fail(EX_FAIL, "配置写入失败：" + c.cfg.path);

  nlohmann::json data{{"login", c.cfg.login}, {"name", c.cfg.name}, {"avatar", c.cfg.avatar},
                      {"config", c.cfg.path}};
  ok(cGreen("✓ 已登录：") + cBold(c.cfg.login) +
         (c.cfg.name.empty() ? "" : "（" + c.cfg.name + "）") + "\n口令保存在 " + cDim(c.cfg.path),
     data);
  return EX_OK;
}

int cmdWhoami(const Args& a) {
  (void)a;
  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  HttpResponse r = c.http.get("https://api.github.com/user",
                              {{"User-Agent", "BlackNet-CLI/0.1"},
                               {"Authorization", "Bearer " + c.token}});
  if (!r.error.empty()) return fail(EX_NET, "网络错误：" + r.error);
  if (r.status == 401) return fail(EX_AUTH, "令牌已失效，请重新 bn login", "auth");
  nlohmann::json u;
  try {
    u = nlohmann::json::parse(r.body);
  } catch (const std::exception&) {
    return fail(EX_FAIL, "GitHub 返回非 JSON");
  }
  nlohmann::json data{{"login", u.value("login", "")},
                      {"name", u.value("name", "")},
                      {"id", u.value("id", 0)},
                      {"token_source", c.cfg.token.empty() ? "env" : "config"}};
  ok(cBold(u.value("login", "?")) +
         (u.value("name", "").empty() ? "" : "（" + u.value("name", "") + "）") + "  " +
         cDim(std::string("token 来源：") + (c.cfg.token.empty() ? "环境变量" : "本地配置")),
     data);
  return EX_OK;
}

int cmdLogout(const Args& a) {
  (void)a;
  Config c = loadConfig();
  std::string p = configPath();
  DeleteFileW(utf8ToWide(p).c_str());
  ok("✓ 已清除本地登录信息（" + p + "）", nlohmann::json{{"removed", p}});
  return EX_OK;
}

/* ================= grp ================= */
int cmdGrpList(const Args& a) {
  Ctx c = makeCtx();
  long n = a.num("n", 20);
  long limit = a.num("limit", 0);
  std::string after = a.str("after");
  std::string catFilter = a.str("cat");
  std::string tagFilter = a.str("tag");
  std::string sort = lowerAscii(a.str("sort", "default"));
  bool asc = a.has("asc");
  bool all = a.has("all");
  bool deep = a.has("deep") || sort == "likes" || sort == "updated" || sort == "hot";

  /* 需要客户端排序时多取一些样本：默认 n*5（上限 200），--all 时一直翻到底（上限 500）*/
  long want = n;
  if (all) want = limit > 0 ? limit : 500;
  else if (deep) want = std::min<long>(n * 5, 200);
  want = std::max<long>(want, 1);

  nlohmann::json j;
  std::string err;
  if (!grpList(c, (int)std::min<long>(want, 100), after, j, err)) return fail(errCode(err), err);

  struct Item {
    nlohmann::json raw;
    std::string title, cat, info, author, created, updated;
    int catIdNum = 0;
    int upvotes = 0;
    bool hasCat = false;
    std::vector<std::string> tags;
    bool pinned = false;
  };
  std::vector<Item> items;
  auto collect = [&](const nlohmann::json& nodes) {
    for (auto& d : nodes) {
      Item it;
      it.raw = d;
      PostMeta m = parsePostBody(d.value("body", ""));
      it.hasCat = m.hasCategory;
      it.catIdNum = m.hasCategory ? m.category : 0;
      it.cat = m.hasCategory ? catName(m.category) : "";
      it.tags = m.tags;
      it.info = m.info.empty() ? stripFirstLine(d.value("body", "")) : m.info;
      it.title = d.value("title", "");
      it.author = (d.contains("author") && d["author"].is_object()) ? d["author"].value("login", "") : "";
      it.created = d.value("createdAt", "");
      it.updated = d.value("updatedAt", "");
      it.upvotes = d.value("upvoteCount", 0);
      it.pinned = (m.hasCategory && m.category == 1);   // 公告
      items.push_back(std::move(it));
    }
  };
  collect(j.value("nodes", nlohmann::json::array()));

  /* deep 模式继续翻页，直到够数或没有下一页 */
  std::string cursor = j.contains("pageInfo") ? j["pageInfo"].value("endCursor", "") : "";
  bool more = j.contains("pageInfo") && j["pageInfo"].value("hasNextPage", false);
  while (deep && more && (long)items.size() < want) {
    nlohmann::json page;
    std::string e2;
    if (!grpList(c, 100, cursor, page, e2)) break;
    collect(page.value("nodes", nlohmann::json::array()));
    more = page.contains("pageInfo") && page["pageInfo"].value("hasNextPage", false);
    cursor = page.contains("pageInfo") ? page["pageInfo"].value("endCursor", "") : "";
    if (cursor.empty()) break;
  }

  /* 过滤：分类 + 标签 */
  std::vector<Item> filtered;
  int wantCat = catFilter.empty() ? -1 : catId(catFilter);
  if (!catFilter.empty() && wantCat < 0 && catId(catFilter) != 0)
    return fail(EX_USAGE, "未知分类：" + catFilter + "（1公告 2新闻 3资讯 4技术 5活动 6随笔）");
  for (auto& it : items) {
    if (!catFilter.empty()) {
      bool hit = (catFilter == it.cat) || (wantCat > 0 && it.hasCat && it.catIdNum == wantCat);
      if (!hit) continue;
    }
    if (!tagFilter.empty()) {
      bool hit = false;
      for (auto& t : it.tags)
        if (lowerAscii(t) == lowerAscii(tagFilter)) { hit = true; break; }
      if (!hit) continue;
    }
    filtered.push_back(it);
  }

  /* 排序：default（公告置顶 + 时间倒序）/ created / updated / likes */
  bool byDefault = (sort.empty() || sort == "default");
  auto byTime = [](const std::string& x, const std::string& y) { return x > y; };
  std::stable_sort(filtered.begin(), filtered.end(), [&](const Item& x, const Item& y) {
    if (byDefault) {
      if (x.pinned != y.pinned) return x.pinned && !asc;
      return asc ? x.created < y.created : byTime(x.created, y.created);
    }
    if (sort == "likes" || sort == "hot") {
      if (x.upvotes != y.upvotes) return asc ? x.upvotes < y.upvotes : x.upvotes > y.upvotes;
      return byTime(x.created, y.created);
    }
    if (sort == "updated") return asc ? x.updated < y.updated : byTime(x.updated, y.updated);
    return asc ? x.created < y.created : byTime(x.created, y.created);   // created
  });

  if (!all && (long)filtered.size() > n) filtered.resize((size_t)n);

  std::vector<std::vector<std::string>> rows;
  nlohmann::json outItems = nlohmann::json::array();
  for (auto& it : filtered) {
    rows.push_back({padLeft(std::to_string(it.raw.value("number", 0)), 4),
                    it.cat.empty() ? "—" : it.cat, it.title, it.author,
                    isoToLocal(it.created), std::to_string(it.upvotes)});
    outItems.push_back({{"number", it.raw.value("number", 0)},
                        {"title", it.title},
                        {"category", it.cat},
                        {"category_id", it.catIdNum},
                        {"author", it.author},
                        {"created_at", it.created},
                        {"updated_at", it.updated},
                        {"upvotes", it.upvotes},
                        {"pinned", it.pinned},
                        {"info", it.info},
                        {"tags", it.tags},
                        {"url", c.cfg.grpBase + "/blog.html?d=" +
                                    std::to_string(it.raw.value("number", 0))}});
  }
  if (g.json) {
    nlohmann::json data{{"items", outItems}, {"sort", byDefault ? "default" : sort}, {"asc", asc}};
    if (j.contains("pageInfo")) data["page_info"] = j["pageInfo"];
    return ok("", data), EX_OK;
  }
  if (rows.empty()) return ok(cDim("（没有符合条件的文章）")), EX_OK;
  printTable({"#", "分类", "标题", "作者", "时间", "赞"}, rows);
  if (!deep && j.contains("pageInfo") && j["pageInfo"].value("hasNextPage", false)) {
    std::cout << cDim("\n下一页：bn grp list --after " + j["pageInfo"].value("endCursor", "")) << "\n";
  }
  return EX_OK;
}

int cmdGrpShow(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn grp show <编号>");
  int number = 0;
  try { number = std::stoi(a.pos[0]); } catch (...) { return fail(EX_USAGE, "编号必须是数字"); }
  Ctx c = makeCtx();
  nlohmann::json j;
  std::string err;
  if (!grpShow(c, number, (int)a.num("comments", 0), j, err)) {
    // 服务端在文章不存在时返回 500（GraphQL 里 discussion 为 null），换成人话
    if (err.find("内部服务器错误") != std::string::npos || err.find("500") != std::string::npos)
      return fail(EX_NOTFOUND, "没有这篇文章：#" + std::to_string(number), "notfound");
    return fail(errCode(err), err);
  }
  if (!j.contains("discussion") || j["discussion"].is_null())
    return fail(EX_NOTFOUND, "没有这篇文章：#" + std::to_string(number), "notfound");
  auto d = j["discussion"];
  PostMeta m = parsePostBody(d.value("body", ""));
  std::string body = m.rest;

  if (g.json) {
    nlohmann::json data = d;
    data["info"] = m.info;
    data["tags"] = m.tags;
    data["category_id"] = m.hasCategory ? m.category : 0;
    data["category_name"] = m.hasCategory ? catName(m.category) : "";
    data["markdown"] = trim(body);
    data["url"] = c.cfg.grpBase + "/blog.html?d=" + std::to_string(number);
    return ok("", data), EX_OK;
  }

  std::cout << cBold("#" + std::to_string(number) + " " + d.value("title", "")) << "\n";
  std::cout << cDim("作者 " + ((d.contains("author") && d["author"].is_object())
                                  ? d["author"].value("login", "")
                                  : "?") +
                    " · " + isoToLocal(d.value("createdAt", "")) +
                    (m.hasCategory ? " · " + std::string(catName(m.category)) : ""))
            << "\n";
  if (!m.tags.empty()) {
    std::string t;
    for (auto& x : m.tags) { if (!t.empty()) t += "  "; t += "#" + x; }
    std::cout << cCyan(t) << "\n";
  }
  if (!m.info.empty()) std::cout << "\n" << cYellow("简介：" + m.info) << "\n";
  std::cout << "\n" << trim(body) << "\n";

  if (a.num("comments", 0) > 0 && d.contains("comments")) {
    auto cs = d["comments"].value("nodes", nlohmann::json::array());
    std::cout << "\n" << cDim("—— 评论 " + std::to_string(cs.size()) + " 条 ——") << "\n";
    for (auto& cm : cs) {
      std::string who = cm.contains("author") && cm["author"].is_object()
                            ? cm["author"].value("login", "?")
                            : "?";
      std::cout << cBold(who) << cDim(" " + isoToLocal(cm.value("createdAt", ""))) << "\n"
                << cm.value("body", "") << "\n\n";
    }
  }
  return EX_OK;
}

int cmdGrpSearch(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn grp search <关键词>");
  std::string kw = a.pos[0];
  long want = a.num("n", 50);
  Ctx c = makeCtx();
  std::string after;
  long scanned = 0;
  std::vector<std::vector<std::string>> rows;
  nlohmann::json items = nlohmann::json::array();
  while (scanned < want) {
    nlohmann::json j;
    std::string err;
    if (!grpList(c, 50, after, j, err)) return fail(errCode(err), err);
    auto nodes = j.value("nodes", nlohmann::json::array());
    if (nodes.empty()) break;
    for (auto& d : nodes) {
      scanned++;
      PostMeta m = parsePostBody(d.value("body", ""));
      std::string title = d.value("title", "");
      std::string body = d.value("body", "");
      if (!icontains(title, kw) && !icontains(body, kw) && !icontains(m.info, kw)) continue;
      std::string preview = m.info.empty() ? stripFirstLine(body) : m.info;
      rows.push_back({padLeft(std::to_string(d.value("number", 0)), 4), title,
                      truncDisplay(preview, 40)});
      items.push_back({{"number", d.value("number", 0)},
                       {"title", title},
                       {"info", preview},
                       {"created_at", d.value("createdAt", "")}});
    }
    if (!j.contains("pageInfo") || !j["pageInfo"].value("hasNextPage", false)) break;
    after = j["pageInfo"].value("endCursor", "");
    if (after.empty()) break;
  }
  if (g.json) return ok("", nlohmann::json{{"keyword", kw}, {"items", items}, {"scanned", scanned}}), EX_OK;
  if (rows.empty()) return ok(cDim("没有匹配「" + kw + "」的文章（已扫 " + std::to_string(scanned) + " 篇）")), EX_OK;
  printTable({"#", "标题", "简介"}, rows);
  std::cout << cDim("\n已扫 " + std::to_string(scanned) + " 篇，命中 " + std::to_string(rows.size()) + " 篇") << "\n";
  return EX_OK;
}

int cmdGrpPost(const Args& a) {
  std::string title = trim(a.str("title"));
  std::string file = a.str("file");
  if (title.empty()) return fail(EX_USAGE, "缺标题：-t \"标题\"");
  if (file.empty()) return fail(EX_USAGE, "缺正文：-f 文件.md（或 -f - 读 stdin）");
  std::string md = file == "-" ? readStdinAll() : readFile(file);
  if (trim(md).empty()) return fail(EX_USAGE, "正文为空：" + file);
  if (displayWidth(title) > 120) return fail(EX_USAGE, "标题过长（上限 120 字）");

  std::string info = trim(a.str("info", "无简介"));
  std::vector<std::string> tags = splitAny(a.str("tags"), ",，、");
  std::string cat = trim(a.str("cat"));
  int catNum = cat.empty() ? -1 : catId(cat);
  if (!cat.empty() && catNum < 0) return fail(EX_USAGE, "未知分类：" + cat + "（1公告 2新闻 3资讯 4技术 5活动 6随笔）");

  nlohmann::json head;
  head["info"] = b64encode(info);
  std::string icon = trim(a.str("cover"));
  head["icon"] = icon.empty() ? "" : b64encode(icon);
  std::string coverText = trim(a.str("cover-text"));
  head["coverText"] = coverText;
  if (catNum > 0) head["category"] = catNum;
  head["tpl"] = a.has("tpl");
  head["allowComments"] = !a.has("no-comments");
  head["tags"] = tags;

  std::string fullBody = head.dump() + "\n\n" + trim(md) + "\n";

  if (a.has("dry")) {
    if (g.json)
      return ok("", nlohmann::json{{"dry_run", true}, {"title", title}, {"body", fullBody},
                                   {"category_id", catNum}}),
             EX_OK;
    std::cout << cDim("（dry-run，不会真的提交）") << "\n" << head.dump() << "\n\n"
              << trim(md) << "\n";
    return EX_OK;
  }

  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  nlohmann::json out;
  std::string err;
  if (!grpPost(c, title, fullBody, out, err)) return fail(errCode(err), err);

  long num = 0;
  if (out.contains("discussion") && out["discussion"].is_object())
    num = out["discussion"].value("number", 0L);
  std::string url = c.cfg.grpBase + "/blog.html?d=" + std::to_string(num);
  ok(cGreen("✓ 已发布") + (num ? " #" + std::to_string(num) : "") + "\n" + url,
     nlohmann::json{{"number", num}, {"url", url}, {"title", title}, {"category_id", catNum},
                    {"tags", tags}});
  return EX_OK;
}

/* 从服务端返回里安全取 discussion.id：字段缺失或类型不对时返回空串，
   而不是让 nlohmann 抛 type_error 把进程 abort 掉（mock / 后端改结构时踩过）。 */
static std::string discussionIdOf(const nlohmann::json& j) {
  if (j.is_object() && j.contains("discussion") && j["discussion"].is_object())
    return j["discussion"].value("id", "");
  return "";
}

int cmdGrpComment(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn grp comment <编号> -m 内容");
  int number = 0;
  try { number = std::stoi(a.pos[0]); } catch (...) { return fail(EX_USAGE, "编号必须是数字"); }
  std::string msg = a.str("message");
  if (msg.empty() && a.has("file")) msg = readFile(a.str("file"));
  if (trim(msg).empty()) return fail(EX_USAGE, "评论内容为空（-m 或 -f）");

  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  nlohmann::json j;
  std::string err;
  if (!grpShow(c, number, 0, j, err)) return fail(errCode(err), err);
  std::string did = discussionIdOf(j);
  if (did.empty()) return fail(EX_FAIL, "服务端返回里没有 discussion.id，没法评论");
  nlohmann::json out;
  if (!grpComment(c, did, msg, a.str("reply"), out, err)) return fail(errCode(err), err);
  ok(cGreen("✓ 评论已发布") + " → " + c.cfg.grpBase + "/blog.html?d=" + std::to_string(number),
     nlohmann::json{{"number", number}, {"ok", true}});
  return EX_OK;
}

int cmdGrpReact(const Args& a) {
  if (a.pos.size() < 2) return fail(EX_USAGE, "用法：bn grp react <编号> <表情>");
  int number = 0;
  try { number = std::stoi(a.pos[0]); } catch (...) { return fail(EX_USAGE, "编号必须是数字"); }
  static const std::map<std::string, std::string> map = {
      {"+1", "THUMBS_UP"},      {"thumbs_up", "THUMBS_UP"}, {"-1", "THUMBS_DOWN"},
      {"thumbs_down", "THUMBS_DOWN"}, {"laugh", "LAUGH"},    {"hooray", "HOORAY"},
      {"confused", "CONFUSED"}, {"heart", "HEART"},          {"rocket", "ROCKET"},
      {"eyes", "EYES"},         {"👍", "THUMBS_UP"},         {"❤", "HEART"},
      {"🎉", "HOORAY"},         {"🚀", "ROCKET"},            {"👀", "EYES"},
      {"😄", "LAUGH"},          {"😕", "CONFUSED"},          {"👎", "THUMBS_DOWN"}};
  std::string key = lowerAscii(a.pos[1]);
  auto it = map.find(key);
  if (it == map.end()) return fail(EX_USAGE, "不认识的表情：" + a.pos[1]);
  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  nlohmann::json j, out;
  std::string err;
  if (!grpShow(c, number, 0, j, err)) return fail(errCode(err), err);
  std::string sid = discussionIdOf(j);
  if (sid.empty()) return fail(EX_FAIL, "服务端返回里没有 discussion.id，没法操作反应");
  if (!grpReact(c, sid, it->second, a.has("remove"), out, err)) return fail(errCode(err), err);
  ok(cGreen(a.has("remove") ? "✓ 已取消反应 " : "✓ 已添加反应 ") + it->second + " → #" + std::to_string(number),
     nlohmann::json{{"number", number}, {"reaction", it->second}, {"removed", a.has("remove")}});
  return EX_OK;
}

int cmdGrpUpvote(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn grp upvote <编号> [--remove]");
  int number = 0;
  try { number = std::stoi(a.pos[0]); } catch (...) { return fail(EX_USAGE, "编号必须是数字"); }
  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  nlohmann::json j, out;
  std::string err;
  if (!grpShow(c, number, 0, j, err)) return fail(errCode(err), err);
  std::string sid = discussionIdOf(j);
  if (sid.empty()) return fail(EX_FAIL, "服务端返回里没有 discussion.id，没法点赞");
  if (!grpUpvote(c, sid, a.has("remove"), out, err)) return fail(errCode(err), err);
  ok(cGreen(a.has("remove") ? "✓ 已取消点赞 " : "✓ 已点赞 ") + "#" + std::to_string(number),
     nlohmann::json{{"number", number}, {"removed", a.has("remove")}});
  return EX_OK;
}

int cmdGrpDelete(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn grp delete <编号>");
  int number = 0;
  try { number = std::stoi(a.pos[0]); } catch (...) { return fail(EX_USAGE, "编号必须是数字"); }
  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  nlohmann::json j;
  std::string err;
  if (!grpShow(c, number, 0, j, err)) return fail(errCode(err), err);
  auto d = j["discussion"];
  std::string title = d.value("title", "");
  if (!confirm("确认删除 #" + std::to_string(number) + "「" + title + "」？不可恢复。", g.yes))
    return fail(EX_USAGE, "已取消");
  if (!grpDelete(c, d.value("id", ""), number, err)) return fail(errCode(err), err);
  ok(cGreen("✓ 已删除 #" + std::to_string(number)) , nlohmann::json{{"number", number}, {"deleted", true}});
  return EX_OK;
}

int cmdStatus(const Args& a) {
  Ctx c = makeCtx();
  StatusSummary s;
  std::string err;
  if (!loadStatusSummary(c, s, err)) return fail(errCode(err), err);

  long incN = a.num("incidents", 3);
  bool hist = a.has("hist");

  if (g.json) {
    nlohmann::json svcs = nlohmann::json::array();
    for (auto& v : s.svcs) {
      nlohmann::json inc = nlohmann::json::array();
      for (auto& x : v.incidents)
        inc.push_back({{"from", x.from}, {"to", x.to}, {"minutes", (x.to - x.from) / 60000}});
      svcs.push_back({{"id", v.id},          {"name", v.name},
                      {"short", v.shortName}, {"group", v.group},
                      {"level", v.level},     {"ok", v.ok},
                      {"ms", v.ms},           {"err", v.err},
                      {"h24_pct", v.h24pct},  {"h24_samples", v.h24n},
                      {"h24_slow", v.h24slow},{"h24_down", v.h24down},
                      {"d7_pct", v.d7pct},    {"d7_samples", v.d7n},
                      {"bad_now", v.badNow},  {"since", v.sinceTs},
                      {"last_up", v.lastUpTs},{"bad_count", v.badCount},
                      {"incidents", inc}});
    }
    nlohmann::json week = nlohmann::json::array();
    for (size_t i = 0; i < s.svcs.size(); i++)
      week.push_back(s.week[i]);
    return ok("", nlohmann::json{{"level", s.level},
                                 {"updated", s.updatedMs},
                                 {"interval_sec", s.intervalSec},
                                 {"source", s.source},
                                 {"days", s.days},
                                 {"week", week},
                                 {"services", svcs}}),
           EX_OK;
  }

  auto levelText = [](const std::string& lv) {
    if (lv == "up") return std::string("正常");
    if (lv == "slow") return std::string("偏慢");
    if (lv == "partial") return std::string("部分异常");
    return std::string("异常");
  };
  auto levelColor = [](const std::string& lv) {
    if (lv == "up") return cGreen("正常");
    if (lv == "slow") return cYellow("偏慢");
    if (lv == "partial") return cYellow("部分异常");
    return cRed("异常");
  };
  auto rateColor = [](double p) {
    std::string t = fmtPct(p);
    if (p < 0) return cDim(t);
    if (p >= 99.95) return cGreen(t);
    if (p >= 99) return cYellow(t);
    return cRed(t);
  };

  std::cout << cBold("群档案 · 服务状态") << "   " << levelColor(s.level)
            << cDim("  · 更新于 " + fmtLocalMs(s.updatedMs) + " · 每 " +
                    std::to_string(s.intervalSec / 60) + " 分钟探测")
            << "\n\n";

  std::vector<std::vector<std::string>> rows;
  for (auto& v : s.svcs) {
    std::string state = levelText(v.level);
    std::string ms = v.ms > 0 ? std::to_string(v.ms) + "ms" : "—";
    std::string note = v.err;
    if (v.badNow && v.sinceTs > 0)
      note = "自 " + fmtLocalMs(v.sinceTs) + " 起异常（" + fmtDuration(v.sinceTs ? nowMs() - v.sinceTs : 0) + "）";
    rows.push_back({cBold(v.shortName), state, ms, fmtPct(v.h24pct), fmtPct(v.d7pct), note});
  }
  printTable({"服务", "状态", "延迟", "24h", "7天", "备注"}, rows);

  /* 近期故障段 */
  std::vector<std::vector<std::string>> bad;
  for (auto& v : s.svcs) {
    int shown = 0;
    for (auto& x : v.incidents) {
      if (shown >= incN) break;
      std::string from = fmtLocalMs(x.from), to = fmtLocalMs(x.to);
      std::string dur = fmtDuration(x.to - x.from);
      std::string tail = (x.to >= v.lastUpTs) ? "（持续中）" : "";
      bad.push_back({v.shortName, from, to, dur, tail});
      shown++;
    }
  }
  if (!bad.empty() && incN > 0) {
    std::cout << "\n" << cYellow("近期故障（近 24 小时）") << "\n";
    printTable({"服务", "开始", "结束", "时长", ""}, bad);
  } else if (incN > 0) {
    std::cout << "\n" << cGreen("近 24 小时没有故障。") << "\n";
  }

  /* 近 7 天矩阵 */
  if (hist) {
    if (s.days.empty()) {
      std::cout << "\n" << cDim("还没有按天的历史数据。") << "\n";
    } else {
      std::cout << "\n" << cYellow("近 7 天可用率") << cDim("（空格分隔，% ）") << "\n";
      std::vector<std::vector<std::string>> w;
      std::vector<std::string> head{"服务"};
      for (auto& d : s.days) head.push_back(d.substr(5));
      for (size_t i = 0; i < s.svcs.size(); i++) {
        std::vector<std::string> row{s.svcs[i].shortName};
        for (size_t d = 0; d < s.days.size(); d++) row.push_back(fmtPct(s.week[i][d]));
        w.push_back(row);
      }
      printTable(head, w);
    }
  }

  std::cout << "\n" << cDim("图形版：https://status.blacknet.cc.cd/") << "\n";
  return EX_OK;
}

/* 按年月归档：bn grp archive [YYYY-MM]，不写参数则列出各月篇数 */
int cmdGrpArchive(const Args& a) {
  Ctx c = makeCtx();
  std::string want = a.pos.empty() ? "" : trim(a.pos[0]);
  long cap = a.num("limit", 500);

  std::vector<nlohmann::json> posts;
  std::string cursor;
  std::string err;
  while ((long)posts.size() < cap) {
    nlohmann::json j;
    if (!grpList(c, 100, cursor, j, err)) {
      if (posts.empty()) return fail(errCode(err), err);
      break;
    }
    auto nodes = j.value("nodes", nlohmann::json::array());
    for (auto& d : nodes) posts.push_back(d);
    if (!j.contains("pageInfo") || !j["pageInfo"].value("hasNextPage", false)) break;
    cursor = j["pageInfo"].value("endCursor", "");
    if (cursor.empty()) break;
  }

  std::map<std::string, std::vector<const nlohmann::json*>> byMonth;
  for (auto& d : posts) {
    std::string t = d.value("createdAt", "");
    if (t.size() < 7) continue;
    byMonth[t.substr(0, 7)].push_back(&d);
  }

  if (g.json) {
    nlohmann::json out = nlohmann::json::object();
    for (auto it = byMonth.rbegin(); it != byMonth.rend(); ++it) {
      nlohmann::json arr = nlohmann::json::array();
      for (auto* p : it->second)
        arr.push_back({{"number", p->value("number", 0)},
                       {"title", p->value("title", "")},
                       {"created_at", p->value("createdAt", "")}});
      out[it->first] = arr;
    }
    return ok("", out), EX_OK;
  }

  if (want.empty()) {
    std::vector<std::vector<std::string>> rows;
    for (auto it = byMonth.rbegin(); it != byMonth.rend(); ++it)
      rows.push_back({it->first, std::to_string(it->second.size())});
    printTable({"月份", "篇数"}, rows);
    std::cout << cDim("\n看某个月：bn grp archive 2026-11") << "\n";
    return EX_OK;
  }

  auto it = byMonth.find(want);
  if (it == byMonth.end()) return fail(EX_NOTFOUND, "这个月没有文章：" + want, "notfound");
  std::vector<std::vector<std::string>> rows;
  for (auto* p : it->second) {
    PostMeta m = parsePostBody(p->value("body", ""));
    rows.push_back({padLeft(std::to_string(p->value("number", 0)), 4),
                    m.hasCategory ? catName(m.category) : "—", p->value("title", ""),
                    isoToLocal(p->value("createdAt", ""))});
  }
  printTable({"#", "分类", "标题", "时间"}, rows);
  return EX_OK;
}

int cmdGrpFeed(const Args& a) {
  Ctx c = makeCtx();
  std::string xml, err;
  if (!grpFeed(c, (int)a.num("n", 20), xml, err)) return fail(errCode(err), err);
  std::vector<std::vector<std::string>> rows;
  nlohmann::json items = nlohmann::json::array();
  size_t pos = 0;
  int count = 0;
  while ((pos = xml.find("<item>", pos)) != std::string::npos && count < 100) {
    size_t end = xml.find("</item>", pos);
    if (end == std::string::npos) break;
    std::string item = xml.substr(pos, end - pos);
    auto tag = [&](const std::string& t) {
      size_t a1 = item.find("<" + t + ">");
      size_t b1 = item.find("</" + t + ">");
      if (a1 == std::string::npos || b1 == std::string::npos) return std::string();
      return item.substr(a1 + t.size() + 2, b1 - a1 - t.size() - 2);
    };
    std::string title = tag("title"), link = tag("link"), date = tag("pubDate");
    size_t q = link.find("d=");
    std::string num = q == std::string::npos ? "" : link.substr(q + 2);
    rows.push_back({padLeft(num, 4), title, date});
    items.push_back({{"number", num}, {"title", title}, {"link", link}, {"pub_date", date}});
    pos = end;
    count++;
  }
  if (g.json) return ok("", nlohmann::json{{"items", items}}), EX_OK;
  if (rows.empty()) return ok(cDim("feed 为空")), EX_OK;
  printTable({"#", "标题", "时间"}, rows);
  return EX_OK;
}

/* ================= book ================= */
struct BookEntry {
  nlohmann::json raw;
  std::string id;
  std::string file;
};

bool findBook(Ctx& c, const std::string& idOrFile, BookEntry& e, std::string& err) {
  nlohmann::json idx;
  if (!bookIndex(c, idx, err)) return false;
  auto books = idx.value("books", nlohmann::json::array());
  for (auto& b : books) {
    std::string id = b.value("id", "");
    std::string file = b.value("file", id + ".book");
    if (id == idOrFile || file == idOrFile || id == idOrFile + ".book") {
      e.raw = b;
      e.id = id;
      e.file = file;
      return true;
    }
  }
  err = "书库里没有这本书：" + idOrFile;
  return false;
}

int cmdBookList(const Args& a) {
  Ctx c = makeCtx();
  nlohmann::json idx;
  std::string err;
  if (!bookIndex(c, idx, err)) return fail(errCode(err), err);
  std::string cat = a.str("cat"), status = a.str("status"), q = a.str("q");
  std::vector<std::vector<std::string>> rows;
  nlohmann::json items = nlohmann::json::array();
  for (auto& b : idx.value("books", nlohmann::json::array())) {
    auto cats = cleanCats(b);
    std::string catText;
    for (size_t i = 0; i < cats.size(); i++) catText += (i ? "、" : "") + cats[i];
    if (!cat.empty()) {
      bool hit = std::find(cats.begin(), cats.end(), cat) != cats.end();
      if (!hit) continue;
    }
    if (!status.empty() && b.value("status", "ongoing") != status) continue;
    if (!q.empty()) {
      std::string blob = b.value("title", "") + " " + b.value("intro", "") + " " +
                         b.value("author", "") + " " + b.dump();
      if (!icontains(blob, q)) continue;
    }
    std::string id = b.value("id", "");
    rows.push_back({id, b.value("title", ""), b.value("author", ""),
                    catText.empty() ? "—" : catText, std::to_string(b.value("chapters", 0)),
                    std::to_string(b.value("words", 0)),
                    b.value("status", "ongoing") == "done" ? "完结" : "连载",
                    isoToLocal(b.value("updatedAt", ""))});
    items.push_back({{"id", id},
                     {"title", b.value("title", "")},
                     {"author", b.value("author", "")},
                     {"categories", cats},
                     {"tags", b.value("tags", nlohmann::json::array())},
                     {"status", b.value("status", "ongoing")},
                     {"visibility", b.value("visibility", "public")},
                     {"chapters", b.value("chapters", 0)},
                     {"words", b.value("words", 0)},
                     {"updated_at", b.value("updatedAt", "")},
                     {"intro", b.value("intro", "")},
                     {"url", c.cfg.bookBase + "/book.html?id=" + id}});
  }
  if (g.json) return ok("", nlohmann::json{{"items", items}, {"total", items.size()}}), EX_OK;
  if (rows.empty()) return ok(cDim("（没有符合条件的书）")), EX_OK;
  printTable({"书号", "书名", "作者", "分类", "章", "字数", "状态", "更新"}, rows);
  return EX_OK;
}

int cmdBookSearch(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn book search <关键词>");
  Args a2 = a;
  a2.val["q"] = a.pos[0];
  return cmdBookList(a2);
}

int cmdBookShow(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn book show <书号>");
  Ctx c = makeCtx();
  BookEntry e;
  std::string err;
  if (!findBook(c, a.pos[0], e, err)) return fail(errCode(err), err);

  std::string secret = e.raw.value("visibility", "public") == "private" ? "" : siteKey();
  if (a.has("password")) secret = a.str("password");
  if (secret.empty()) {
    std::cout << "这是一本私密书，需要阅读口令。\n";
    secret = promptHidden("口令> ");
    if (secret.empty()) return fail(EX_USAGE, "没有口令");
  }

  std::string text;
  if (!bookFetch(c, e.id, e.file, text, err)) return fail(errCode(err), err);
  BookFile bf;
  if (!bookUnpack(text, secret, bf, err)) return fail(EX_FAIL, err);

  auto chapters = bf.chapters;
  std::string title = bf.meta.value("title", e.id);
  std::string author = bf.meta.value("author", "");
  long chapterArg = a.num("chapter", 0);

  if (g.json) {
    nlohmann::json data{{"id", e.id},
                        {"title", title},
                        {"author", author},
                        {"meta", bf.meta},
                        {"words", bf.words},
                        {"chapters", nlohmann::json::array()}};
    for (size_t i = 0; i < chapters.size(); i++) {
      auto ch = chapters[i];
      nlohmann::json item{{"index", (int)i + 1},
                          {"id", ch.value("id", "")},
                          {"title", ch.value("title", "")},
                          {"words", countWords(ch.value("content", ""))}};
      if (chapterArg == 0 || chapterArg == (long)i + 1) item["content"] = ch.value("content", "");
      data["chapters"].push_back(item);
    }
    return ok("", data), EX_OK;
  }

  if (chapterArg > 0) {
    if (chapterArg > (long)chapters.size())
      return fail(EX_NOTFOUND, "没有第 " + std::to_string(chapterArg) + " 章（共 " +
                                  std::to_string(chapters.size()) + " 章）");
    auto ch = chapters[(size_t)chapterArg - 1];
    if (!a.has("raw"))
      std::cout << cBold("第 " + std::to_string(chapterArg) + " 章 · " + ch.value("title", "")) << "\n\n";
    std::cout << ch.value("content", "") << "\n";
    return EX_OK;
  }

  std::cout << cBold(title) << cDim("  " + author) << "\n";
  std::cout << cDim(std::to_string(chapters.size()) + " 章 · " + std::to_string(bf.words) + " 字 · " +
                    bf.meta.value("status", "ongoing") + "\n");
  if (!bf.meta.value("intro", "").empty()) std::cout << "\n" << bf.meta.value("intro", "") << "\n";
  std::cout << "\n";
  std::vector<std::vector<std::string>> rows;
  for (size_t i = 0; i < chapters.size(); i++) {
    auto ch = chapters[i];
    rows.push_back({padLeft(std::to_string(i + 1), 3), ch.value("title", ""),
                    std::to_string(countWords(ch.value("content", ""))) + " 字"});
  }
  printTable({"#", "章节", "字数"}, rows);
  std::cout << cDim("\n读某一章：bn book show " + e.id + " --chapter 1") << "\n";
  return EX_OK;
}

int cmdBookGet(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn book get <书号> [-o 输出文件]");
  Ctx c = makeCtx();
  BookEntry e;
  std::string err;
  if (!findBook(c, a.pos[0], e, err)) return fail(errCode(err), err);
  std::string text;
  if (!bookFetch(c, e.id, e.file, text, err)) return fail(errCode(err), err);
  std::string out = a.str("out", e.file);
  if (!writeFile(out, text)) return fail(EX_FAIL, "写入失败：" + out);
  ok(cGreen("✓ 已保存 ") + out + cDim("（" + humanBytes(text.size()) + "）"),
     nlohmann::json{{"file", out}, {"bytes", text.size()}, {"id", e.id}});
  return EX_OK;
}

/* 章节文件名：0001-标题.md */
static std::string safeName(std::string s) {
  for (auto& c : s)
    if (strchr("\\/:*?\"<>|", c)) c = '_';
  return trim(s).empty() ? "untitled" : s;
}

int cmdBookUnpack(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn book unpack <文件.book> [-o 目录]");
  std::string raw = readFile(a.pos[0]);
  if (raw.empty()) return fail(EX_USAGE, "读不到文件：" + a.pos[0]);
  std::string secret = a.has("password") ? a.str("password")
                                         : (raw.find("\"wrap\":\"pass\"") != std::string::npos ? "" : siteKey());
  if (secret.empty()) secret = promptHidden("口令> ");
  BookFile bf;
  std::string err;
  if (!bookUnpack(raw, secret, bf, err)) return fail(EX_FAIL, err);

  std::string dir = a.str("out", bf.meta.value("id", "book"));
  MakeSureDir(dir);
  nlohmann::json meta = bf.meta;
  meta["cli_unpacked_at"] = nowIso();
  writeFile(dir + "\\meta.json", meta.dump(2) + "\n");
  int i = 0;
  nlohmann::json list = nlohmann::json::array();
  for (auto& ch : bf.chapters) {
    i++;
    char pre[16];
    snprintf(pre, sizeof(pre), "%04d-", i);
    std::string name = std::string(pre) + safeName(ch.value("title", "untitled")) + ".md";
    writeFile(dir + "\\" + name, ch.value("content", ""));
    list.push_back({{"index", i}, {"file", name}, {"title", ch.value("title", "")}});
  }
  ok(cGreen("✓ 已解开 ") + bf.meta.value("title", "") + " → " + dir + cDim("（" +
             std::to_string(i) + " 章）"),
     nlohmann::json{{"dir", dir}, {"chapters", list}, {"words", bf.words}});
  return EX_OK;
}

int cmdBookPack(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn book pack <目录> [-o out.book] [--meta meta.json]");
  std::string dir = a.pos[0];
  nlohmann::json meta;
  std::string metaFile = a.str("meta", dir + "\\meta.json");
  std::string metaRaw = readFile(metaFile);
  if (!metaRaw.empty()) {
    try { meta = nlohmann::json::parse(metaRaw); } catch (...) { return fail(EX_USAGE, "meta 不是合法 JSON：" + metaFile); }
  }
  if (meta.is_null() || !meta.is_object()) meta = nlohmann::json::object();

  // 收集 *.md（按文件名排序）
  std::vector<std::string> files;
  WIN32_FIND_DATAW fd;
  std::wstring pat = utf8ToWide(dir + "\\*.md");
  HANDLE h = FindFirstFileW(pat.c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        files.push_back(utf8ToWideLossless(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) return fail(EX_USAGE, "目录里没有 .md：  " + dir);

  nlohmann::json chapters = nlohmann::json::array();
  int idx = 0;
  for (auto& f : files) {
    idx++;
    std::string content = readFile(dir + "\\" + f);
    std::string title = f;
    size_t dot = title.rfind(".md");
    if (dot != std::string::npos) title = title.substr(0, dot);
    size_t dash = title.find('-');
    if (dash != std::string::npos && dash + 1 < title.size()) title = title.substr(dash + 1);
    chapters.push_back({{"id", "c" + std::to_string(idx)}, {"title", title}, {"content", content}});
  }

  std::string id = meta.value("id", "");
  if (id.empty()) id = newBookId();
  meta["id"] = id;
  nlohmann::json book{{"id", id}, {"meta", meta}, {"chapters", chapters}};

  std::string secret = siteKey();
  bool byPass = false;
  if (a.has("password")) { secret = a.str("password"); byPass = true; }
  std::string out, err;
  if (!bookPack(book, secret, byPass, out, err)) return fail(EX_FAIL, err);

  std::string file = a.str("out", id + ".book");
  if (!writeFile(file, out)) return fail(EX_FAIL, "写入失败：" + file);
  ok(cGreen("✓ 已打包 ") + file + cDim("（" + humanBytes(out.size()) + "，" +
             std::to_string(chapters.size()) + " 章）"),
     nlohmann::json{{"file", file}, {"id", id}, {"chapters", chapters.size()}, {"bytes", out.size()}});
  return EX_OK;
}

int cmdBookPublish(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn book publish <文件.book|目录>");
  std::string input = a.pos[0];
  std::string bookJson, err, id, file;

  bool isDir = !fileExists(input);
  if (isDir) {
    if (!fileExists(input + "\\meta.json") && !a.has("meta"))
      return fail(EX_USAGE, "目录下缺 meta.json，或用 --meta 指定");
    // 直接内联打包
    nlohmann::json meta;
    std::string metaRaw = readFile(a.str("meta", input + "\\meta.json"));
    if (!metaRaw.empty()) {
      try { meta = nlohmann::json::parse(metaRaw); } catch (...) { return fail(EX_USAGE, "meta 不是合法 JSON"); }
    }
    if (!meta.is_object()) meta = nlohmann::json::object();
    std::vector<std::string> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(utf8ToWide(input + "\\*.md").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(utf8ToWideLossless(fd.cFileName));
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) return fail(EX_USAGE, "目录里没有 .md：" + input);
    nlohmann::json chapters = nlohmann::json::array();
    int i = 0;
    for (auto& f : files) {
      i++;
      std::string title = f.substr(0, f.rfind(".md"));
      size_t dash = title.find('-');
      if (dash != std::string::npos && dash + 1 < title.size()) title = title.substr(dash + 1);
      chapters.push_back({{"id", "c" + std::to_string(i)}, {"title", title},
                          {"content", readFile(input + "\\" + f)}});
    }
    id = meta.value("id", "");
    if (id.empty()) id = newBookId();
    meta["id"] = id;
    std::string secret = a.has("password") ? a.str("password") : siteKey();
    if (!bookPack(nlohmann::json{{"id", id}, {"meta", meta}, {"chapters", chapters}}, secret,
                  a.has("password"), bookJson, err))
      return fail(EX_FAIL, err);
    file = id + ".book";
  } else {
    bookJson = readFile(input);
    if (bookJson.empty()) return fail(EX_USAGE, "读不到文件：" + input);
    try {
      auto j = nlohmann::json::parse(bookJson);
      if (!j.contains("meta")) return fail(EX_USAGE, "不是 .book 文件：" + input);
      id = j["meta"].value("id", "");
      file = id + ".book";
    } catch (...) {
      return fail(EX_USAGE, "不是 .book 文件：" + input);
    }
  }

  // 组装清单条目
  BookFile bf;
  std::string secret = a.has("password") ? a.str("password") : siteKey();
  if (!bookUnpack(bookJson, secret, bf, err)) return fail(EX_FAIL, "本地校验失败：" + err);

  nlohmann::json entry = bf.meta;
  if (a.has("meta")) {
    std::string mr = readFile(a.str("meta"));
    if (!mr.empty()) {
      try {
        auto extra = nlohmann::json::parse(mr);
        if (extra.is_object())
          for (auto it = extra.begin(); it != extra.end(); ++it) entry[it.key()] = it.value();
      } catch (...) {}
    }
  }
  entry["id"] = id;
  entry["file"] = file;
  entry["updatedAt"] = nowIso();

  if (a.has("dry")) {
    return ok("", nlohmann::json{{"dry_run", true}, {"id", id}, {"file", file},
                                 {"bytes", bookJson.size()}, {"entry", entry}}),
           EX_OK;
  }

  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  if (!bookPublish(c, bookJson, entry, err,
                   [](int i, int total) {
                     if (!g.json)
                       std::cerr << "\r  上传分片 " << i << "/" << total << std::flush;
                   }))
    return fail(errCode(err), err);
  if (!g.json && !bookJson.empty()) std::cerr << "\r" << std::string(30, ' ') << "\r";

  std::string url = c.cfg.bookBase + "/book.html?id=" + id;
  ok(cGreen("✓ 已发布 ") + bf.meta.value("title", id) + " → " + url,
     nlohmann::json{{"id", id}, {"file", file}, {"url", url},
                    {"chapters", bf.chapters.size()}, {"bytes", bookJson.size()}});
  return EX_OK;
}

int cmdBookCollections(const Args& a) {
  Ctx c = makeCtx();
  std::string sub = a.pos.empty() ? "list" : a.pos[0];

  if (sub == "list") {
    nlohmann::json idx;
    std::string err;
    if (!bookCollections(c, idx, err)) return fail(errCode(err), err);
    auto items = idx.value("items", nlohmann::json::array());
    if (g.json) return ok("", nlohmann::json{{"items", items}}), EX_OK;
    std::vector<std::vector<std::string>> rows;
    for (auto& it : items)
      rows.push_back({it.value("id", ""), it.value("title", ""),
                      std::to_string(it.value("count", 0)) + " 部",
                      it.value("account", ""), isoToLocal(it.value("updatedAt", ""))});
    if (rows.empty()) return ok(cDim("（还没有作品集）")), EX_OK;
    printTable({"id", "名称", "收录", "作者", "更新"}, rows);
    return EX_OK;
  }

  if (sub == "show") {
    if (a.pos.size() < 2) return fail(EX_USAGE, "用法：bn book collections show <id>");
    std::string rel = "docs/collections/" + a.pos[1] + ".json";
    HttpResponse r = c.http.get(c.cfg.bookBase + "/" + rel, {{"User-Agent", "BlackNet-CLI/0.1"}});
    if (r.status != 200) return fail(EX_NOTFOUND, "没有这个作品集：" + a.pos[1]);
    nlohmann::json col;
    try { col = nlohmann::json::parse(r.body); } catch (...) { return fail(EX_FAIL, "作品集 JSON 损坏"); }
    if (g.json) return ok("", col), EX_OK;
    std::cout << cBold(col.value("title", "")) << cDim("  " + col.value("account", "")) << "\n";
    if (!col.value("desc", "").empty()) std::cout << col.value("desc", "") << "\n";
    nlohmann::json idx;
    std::string err;
    std::map<std::string, std::string> titles;
    if (bookIndex(c, idx, err))
      for (auto& b : idx.value("books", nlohmann::json::array())) titles[b.value("id", "")] = b.value("title", "");
    std::cout << "\n";
    int i = 0;
    for (auto& b : col.value("bookIds", nlohmann::json::array())) {
      std::string bid = b.get<std::string>();
      std::string tname = titles.count(bid) ? titles[bid] : "（书库里找不到）";
      std::cout << "  " << ++i << ". " << padRight(bid, 28) << " " << tname << "\n";
    }
    return EX_OK;
  }

  if (sub == "create") {
    if (!requireToken(c)) return EX_AUTH;
    std::string title = a.str("title");
    if (title.empty()) return fail(EX_USAGE, "缺 --title");
    auto books = splitAny(a.str("books"), ",，、 ");
    if (books.empty()) return fail(EX_USAGE, "缺 --books（书号逗号分隔）");
    nlohmann::json col{{"title", title}, {"desc", a.str("desc")}, {"bookIds", books},
                       {"author", c.cfg.name.empty() ? c.cfg.login : c.cfg.name}};
    if (a.has("id")) col["id"] = a.str("id");
    nlohmann::json out;
    std::string err;
    if (!bookCollectionSave(c, col, out, err)) return fail(errCode(err), err);
    std::string cid = out.contains("collection") ? out["collection"].value("id", "") : "";
    ok(cGreen("✓ 作品集已保存 ") + title + cDim(" (" + cid + ")"),
       nlohmann::json{{"id", cid}, {"title", title}, {"books", books}});
    return EX_OK;
  }

  if (sub == "delete") {
    if (a.pos.size() < 2) return fail(EX_USAGE, "用法：bn book collections delete <id>");
    if (!requireToken(c)) return EX_AUTH;
    if (!confirm("确认删除作品集 " + a.pos[1] + "？", g.yes)) return fail(EX_USAGE, "已取消");
    nlohmann::json out;
    std::string err;
    if (!bookCollectionDelete(c, a.pos[1], out, err)) return fail(errCode(err), err);
    ok(cGreen("✓ 已删除 ") + a.pos[1], nlohmann::json{{"id", a.pos[1]}});
    return EX_OK;
  }

  return fail(EX_USAGE, "未知子命令：" + sub + "（list | show | create | delete）");
}

int cmdBookShelf(const Args& a) {
  (void)a;
  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;
  nlohmann::json j;
  std::string err;
  if (!bookShelf(c, j, err)) return fail(errCode(err), err);
  auto shelf = j.value("shelf", nlohmann::json::object());
  auto fav = shelf.value("fav", nlohmann::json::array());
  if (g.json) return ok("", nlohmann::json{{"shelf", shelf}}), EX_OK;
  if (fav.empty()) return ok(cDim("（书架是空的）")), EX_OK;
  std::vector<std::vector<std::string>> rows;
  auto prog = shelf.value("progress", nlohmann::json::object());
  for (auto& f : fav) {
    std::string id = f.value("id", "");
    std::string pct = "—";
    if (prog.contains(id)) {
      double r = prog[id].value("ratio", 0.0);
      pct = std::to_string((int)(r * 100)) + "%";
    }
    rows.push_back({id, f.value("title", ""), f.value("author", ""), pct,
                    isoToLocal(f.value("at", ""))});
  }
  printTable({"书号", "书名", "作者", "进度", "收藏于"}, rows);
  return EX_OK;
}

/* 我的书 / 某个作者的书：按账号过滤书阁索引；profile=true 时先打作者资料 */
int cmdBookMineImpl(const Args& a, bool profile) {
  Ctx c = makeCtx();
  std::string me = a.pos.empty() ? "" : trim(a.pos[0]);
  if (me.empty()) me = c.cfg.login;
  if (me.empty()) me = trim(a.str("author"));
  if (me.empty() && !c.token.empty()) {
    HttpResponse r = c.http.get("https://api.github.com/user",
                                {{"User-Agent", "BlackNet-CLI/1.0"},
                                 {"Authorization", "Bearer " + c.token}});
    if (r.ok()) {
      try {
        me = nlohmann::json::parse(r.body).value("login", "");
      } catch (const std::exception&) {
      }
    }
  }
  if (me.empty())
    return fail(EX_AUTH, "不知道你的账号：先 bn login，或用 --author <账号> 指定", "auth");

  std::string err;
  nlohmann::json prof = nlohmann::json::object();
  if (profile) {
    if (!bookProfile(c, me, prof, err)) prof = nlohmann::json::object();
  }

  nlohmann::json idx;
  if (!bookIndex(c, idx, err)) return fail(errCode(err), err);

  std::vector<std::vector<std::string>> rows;
  nlohmann::json items = nlohmann::json::array();
  for (auto& b : idx.value("books", nlohmann::json::array())) {
    std::string acc = b.value("account", "");
    std::string author = b.value("author", "");
    if (lowerAscii(acc) != lowerAscii(me) && lowerAscii(author) != lowerAscii(me)) continue;
    std::string id = b.value("id", "");
    auto cats = cleanCats(b);
    std::string catText;
    for (size_t i = 0; i < cats.size(); i++) catText += (i ? "、" : "") + cats[i];
    rows.push_back({id, b.value("title", ""), catText.empty() ? "—" : catText,
                    std::to_string(b.value("chapters", 0)), std::to_string(b.value("words", 0)),
                    b.value("status", "ongoing") == "done" ? "完结" : "连载",
                    b.value("visibility", "public") == "private" ? "私密" : "公开",
                    isoToLocal(b.value("updatedAt", ""))});
    items.push_back({{"id", id},
                     {"title", b.value("title", "")},
                     {"categories", cats},
                     {"status", b.value("status", "ongoing")},
                     {"visibility", b.value("visibility", "public")},
                     {"chapters", b.value("chapters", 0)},
                     {"words", b.value("words", 0)},
                     {"updated_at", b.value("updatedAt", "")},
                     {"url", c.cfg.bookBase + "/book.html?id=" + id}});
  }
  if (g.json)
    return ok("", nlohmann::json{{"author", me}, {"profile", prof}, {"items", items}}), EX_OK;
  if (profile && !prof.empty()) {
    std::cout << cBold(prof.value("name", me)) << cDim("  @" + me) << "\n";
    if (!prof.value("bio", "").empty()) std::cout << cYellow(prof.value("bio", "")) << "\n";
    std::cout << "\n";
  }
  if (rows.empty()) return ok(cDim(me + " 名下还没有书。")), EX_OK;
  std::cout << cBold(me) << cDim(" 的书（" + std::to_string(rows.size()) + " 本）") << "\n";
  printTable({"书号", "书名", "分类", "章", "字数", "状态", "可见", "更新"}, rows);
  return EX_OK;
}

int cmdBookMine(const Args& a) { return cmdBookMineImpl(a, false); }
int cmdBookAuthor(const Args& a) {
  if (a.pos.empty()) return fail(EX_USAGE, "用法：bn book author <GitHub 账号>");
  return cmdBookMineImpl(a, true);
}

/* 云端草稿（/api/works）：换设备接着写 */
static std::vector<std::string> listMdFiles(const std::string& dir) {
  std::vector<std::string> files;
  WIN32_FIND_DATAW fd;
  std::wstring pat = utf8ToWide(dir + "\\*.md");
  HANDLE h = FindFirstFileW(pat.c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        files.push_back(utf8ToWideLossless(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(files.begin(), files.end());
  return files;
}

int cmdBookDrafts(const Args& a) {
  Ctx c = makeCtx();
  if (!requireToken(c)) return EX_AUTH;

  std::string sub = a.pos.empty() ? "list" : a.pos[0];
  std::string id = a.pos.size() > 1 ? a.pos[1] : a.str("id");
  std::string err;

  /* 目录 → 推送到云端 */
  auto pushDir = [&](const std::string& dir) -> int {
    std::string metaRaw = readFile(dir + "\\meta.json");
    nlohmann::json meta = nlohmann::json::object();
    if (!metaRaw.empty()) {
      try {
        meta = nlohmann::json::parse(metaRaw);
      } catch (const std::exception&) {
        return fail(EX_USAGE, "meta.json 不是合法 JSON：" + dir);
      }
    }
    std::string bid = a.str("id", meta.value("id", id));
    if (bid.empty()) bid = newBookId();
    meta["id"] = bid;

    auto files = listMdFiles(dir);
    if (files.empty()) return fail(EX_USAGE, "目录里没有 .md：" + dir);
    nlohmann::json chapters = nlohmann::json::array();
    int n = 0;
    for (auto& f : files) {
      n++;
      std::string title = f;
      size_t dot = title.rfind(".md");
      if (dot != std::string::npos) title = title.substr(0, dot);
      size_t dash = title.find('-');
      if (dash != std::string::npos && dash + 1 < title.size()) title = title.substr(dash + 1);
      chapters.push_back({{"id", "c" + std::to_string(n)},
                          {"title", title},
                          {"content", readFile(dir + "\\" + f)}});
    }
    nlohmann::json book{{"id", bid}, {"meta", meta}, {"chapters", chapters}};
    nlohmann::json out;
    if (!bookWorksSave(c, book, out, err)) return fail(errCode(err), "推送草稿失败：" + err);
    if (g.json) return ok("", out), EX_OK;
    std::cout << cGreen("✓ 已推送 ") << bid << cDim("（" + std::to_string(n) + " 章，" +
                                                    std::to_string(out.value("chunks", 1)) +
                                                    " 块，服务端 " +
                                                    std::to_string(out.value("bytes", 0)) + " 字节）")
              << "\n";
    return EX_OK;
  };

  /* 目录检测：drafts <目录> 直接当 push */
  if (!sub.empty()) {
    std::wstring w = utf8ToWide(sub);
    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return pushDir(sub);
  }

  if (sub == "list" || sub == "ls") {
    nlohmann::json j;
    if (!bookWorksList(c, j, err)) return fail(errCode(err), err);
    auto books = j.value("books", nlohmann::json::array());
    if (g.json) return ok("", j), EX_OK;
    if (books.empty()) return ok(cDim("云端没有草稿。用 bn book drafts <目录> 推一本上去。")), EX_OK;
    std::vector<std::vector<std::string>> rows;
    for (auto& b : books) {
      rows.push_back({b.value("id", ""), b.value("title", ""),
                      std::to_string(b.value("chapters", 0)),
                      std::to_string(b.value("words", 0)),
                      std::to_string(b.value("chunks", 1)),
                      b.contains("published") && !b["published"].is_null() ? "已发布" : "未发布",
                      isoToLocal(b.value("updatedAt", b.value("at", "")))});
    }
    printTable({"书号", "书名", "章", "字数", "块", "状态", "更新"}, rows);
    return EX_OK;
  }

  if (sub == "push") {
    if (id.empty()) return fail(EX_USAGE, "用法：bn book drafts push <目录> [--id bk_xxx]");
    return pushDir(id);
  }

  if (sub == "pull" || sub == "get") {
    if (id.empty()) return fail(EX_USAGE, "用法：bn book drafts pull <书号> [-o 目录]");
    nlohmann::json j;
    if (!bookWorksGet(c, id, j, err)) {
      if (err.find("404") != std::string::npos)
        return fail(EX_NOTFOUND, "云端没有这本书的草稿：" + id, "notfound");
      return fail(errCode(err), err);
    }
    if (!j.contains("book") || !j["book"].is_object())
      return fail(EX_FAIL, "云端返回里没有 book");
    auto book = j["book"];
    auto meta = book.value("meta", nlohmann::json::object());
    auto chapters = book.value("chapters", nlohmann::json::array());
    std::string dir = a.str("out", id);
    MakeSureDir(dir);
    meta["cli_pulled_at"] = nowIso();
    writeFile(dir + "\\meta.json", meta.dump(2) + "\n");
    nlohmann::json list = nlohmann::json::array();
    int n = 0;
    for (auto& ch : chapters) {
      n++;
      char pre[16];
      snprintf(pre, sizeof(pre), "%04d-", n);
      std::string name = std::string(pre) + safeName(ch.value("title", "untitled")) + ".md";
      writeFile(dir + "\\" + name, ch.value("content", ""));
      list.push_back({{"index", n}, {"file", name}, {"title", ch.value("title", "")}});
    }
    if (g.json)
      return ok("", nlohmann::json{{"dir", dir}, {"id", id}, {"chapters", list},
                                   {"words", meta.value("words", 0)}}),
             EX_OK;
    std::cout << cGreen("✓ 已拉取 ") << meta.value("title", id) << " → " << dir
              << cDim("（" + std::to_string(n) + " 章）") << "\n";
    return EX_OK;
  }

  if (sub == "show") {
    if (id.empty()) return fail(EX_USAGE, "用法：bn book drafts show <书号>");
    nlohmann::json j;
    if (!bookWorksGet(c, id, j, err)) return fail(errCode(err), err);
    auto book = j.value("book", nlohmann::json::object());
    auto meta = book.value("meta", nlohmann::json::object());
    auto chapters = book.value("chapters", nlohmann::json::array());
    if (g.json) return ok("", j), EX_OK;
    std::cout << cBold(meta.value("title", id)) << cDim("  " + id) << "\n";
    std::cout << cDim("作者 " + meta.value("author", "—") + " · " +
                      std::to_string(chapters.size()) + " 章 · " +
                      std::to_string(meta.value("words", 0)) + " 字 · 更新 " +
                      isoToLocal(book.value("at", "")))
              << "\n";
    if (!meta.value("intro", "").empty()) std::cout << "\n" << cYellow("简介：" + meta.value("intro", "")) << "\n";
    std::cout << "\n";
    std::vector<std::vector<std::string>> rows;
    int n = 0;
    for (auto& ch : chapters)
      rows.push_back({padLeft(std::to_string(++n), 4), ch.value("title", "")});
    printTable({"#", "章节"}, rows);
    return EX_OK;
  }

  if (sub == "rm" || sub == "delete") {
    if (id.empty()) return fail(EX_USAGE, "用法：bn book drafts rm <书号> -y");
    if (!confirm("删除云端草稿 " + id + "？不可恢复。", a.has("yes"))) return EX_FAIL;
    nlohmann::json out;
    if (!bookWorksDelete(c, id, out, err)) return fail(errCode(err), err);
    ok(cGreen("✓ 已删除云端草稿 ") + id, out);
    return EX_OK;
  }

  return fail(EX_USAGE, "未知子命令：bn book drafts " + sub + "（list / show / pull / push / rm）");
}


}  // namespace
}  // namespace bn

/* ================= 入口 ================= */
int main(int argc, char** argv) {
  using namespace bn;
  uiInit();

  // Windows 下 argv 是 ANSI 代码页（中文系统是 GBK），必须换成 UTF-8，
  // 否则「bn grp search 状态页」这种带中文的参数会变成乱码。
  static std::vector<std::string> argStore;
  static std::vector<char*> argPtrs;   // 必须比 argv 活得久
  {
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
      for (int i = 0; i < wargc; i++) argStore.push_back(utf8ToWideLossless(wargv[i]));
      LocalFree(wargv);
      for (auto& s : argStore) argPtrs.push_back(const_cast<char*>(s.c_str()));
      argv = argPtrs.data();
      argc = (int)argStore.size();
    }
  }

  if (argc < 2) {
    // 交互式终端直接进 TUI；被脚本/管道调用时仍然只打印用法（agent 友好）
    if (stdoutIsTty()) return cmdTui();
    printHelp();
    return EX_USAGE;
  }

  // 先扫一遍全局开关（-h 留给子命令判断）
  std::vector<char*> rest;
  for (int i = 0; i < argc; i++) rest.push_back(argv[i]);
  Args gargs = parseArgs(argc - 1, argv + 1);
  if (gargs.has("json")) g.json = true;
  if (gargs.has("no-color")) g.color = false;
  if (gargs.has("verbose")) g.verbose = true;
  if (gargs.has("yes")) g.yes = true;

  std::string domain = argv[1];
  std::string cmd = argc > 2 ? argv[2] : "";

  if (domain == "-h" || domain == "--help" || domain == "help") {
    printHelp();
    return EX_OK;
  }
  if (domain == "--version" || domain == "version" || domain == "-V") {
    std::cout << "bn " << kVersion << "\n";
    return EX_OK;
  }

  Args a = argc > 3 ? parseArgs(argc - 3, argv + 3) : Args{};
  // 一级命令（bn status --hist）的参数从 index 2 开始
  Args a1 = argc > 2 ? parseArgs(argc - 2, argv + 2) : Args{};

  if (domain == "login" || domain == "auth") return cmdLogin(parseArgs(argc - 1, argv + 1));
  if (domain == "logout") return cmdLogout(a);
  if (domain == "whoami") return cmdWhoami(a);
  if (domain == "tui" || domain == "ui" || domain == "term") {
    if (a1.has("dump"))
      return cmdTuiDump(a1.str("dump", "tui-dump.txt"), (int)a1.num("width", 120),
                        (int)a1.num("height", 40));
    return cmdTui();
  }
  if (domain == "status" || domain == "st") return cmdStatus(a1);

  if (domain == "grp") {
    if (cmd.empty() || cmd == "-h" || cmd == "--help" || cmd == "help") { printGrpHelp(); return EX_OK; }
    if (cmd == "list" || cmd == "ls") return cmdGrpList(a);
    if (cmd == "show" || cmd == "get") return cmdGrpShow(a);
    if (cmd == "search" || cmd == "find") return cmdGrpSearch(a);
    if (cmd == "post" || cmd == "new") return cmdGrpPost(a);
    if (cmd == "comment" || cmd == "reply") return cmdGrpComment(a);
    if (cmd == "react" || cmd == "reaction") return cmdGrpReact(a);
    if (cmd == "upvote" || cmd == "like") return cmdGrpUpvote(a);
    if (cmd == "delete" || cmd == "rm") return cmdGrpDelete(a);
    if (cmd == "status" || cmd == "st") return cmdStatus(a);
    if (cmd == "archive" || cmd == "months") return cmdGrpArchive(a);
    if (cmd == "feed" || cmd == "rss") return cmdGrpFeed(a);
    return fail(EX_USAGE, "未知命令：bn grp " + cmd);
  }

  if (domain == "book" || domain == "books") {
    if (cmd.empty() || cmd == "-h" || cmd == "--help" || cmd == "help") { printBookHelp(); return EX_OK; }
    if (cmd == "list" || cmd == "ls") return cmdBookList(a);
    if (cmd == "show" || cmd == "read") return cmdBookShow(a);
    if (cmd == "search" || cmd == "find") return cmdBookSearch(a);
    if (cmd == "get" || cmd == "download") return cmdBookGet(a);
    if (cmd == "unpack" || cmd == "extract") return cmdBookUnpack(a);
    if (cmd == "pack") return cmdBookPack(a);
    if (cmd == "publish" || cmd == "push") return cmdBookPublish(a);
    if (cmd == "collections" || cmd == "col") return cmdBookCollections(a);
    if (cmd == "shelf") return cmdBookShelf(a);
    if (cmd == "mine" || cmd == "my") return cmdBookMine(a);
    if (cmd == "author" || cmd == "who") return cmdBookAuthor(a);
    if (cmd == "drafts" || cmd == "draft") return cmdBookDrafts(a);
    return fail(EX_USAGE, "未知命令：bn book " + cmd);
  }

  printHelp();
  return EX_USAGE;
}
