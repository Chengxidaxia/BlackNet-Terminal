#include "statusview.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

namespace bn {

std::string fmtPct(double v) {
  if (v < 0) return "—";
  char buf[32];
  if (v >= 100) return "100%";
  std::snprintf(buf, sizeof(buf), "%.1f%%", v);
  return buf;
}

std::string fmtLocalMs(int64_t ms, bool withDate) {
  if (ms <= 0) return "—";
  time_t t = (time_t)(ms / 1000);
  struct tm tmv;
  localtime_s(&tmv, &t);
  char buf[64];
  if (withDate) std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tmv);
  else std::strftime(buf, sizeof(buf), "%H:%M", &tmv);
  return buf;
}

std::string fmtDuration(int64_t ms) {
  if (ms <= 0) return "—";
  int64_t sec = ms / 1000;
  if (sec < 60) return std::to_string(sec) + " 秒";
  int64_t min = sec / 60;
  if (min < 60) return std::to_string(min) + " 分";
  int64_t h = min / 60, mm = min % 60;
  if (h < 24)
    return std::to_string(h) + " 小时" + (mm ? " " + std::to_string(mm) + " 分" : std::string());
  int64_t d = h / 24, hh = h % 24;
  return std::to_string(d) + " 天" + (hh ? " " + std::to_string(hh) + " 小时" : std::string());
}

static double pctOf(long long up, long long total) {
  if (total <= 0) return -1;
  return std::round((double)up / (double)total * 1000.0) / 10.0;
}

void computeStatus(const nlohmann::json& doc, StatusSummary& out) {
  out = StatusSummary();
  out.ok = true;
  out.updatedMs = doc.value("updated", (long long)0);
  out.intervalSec = doc.value("intervalSec", 120);
  out.source = doc.value("source", "");

  auto svcs = doc.value("services", nlohmann::json::array());
  auto sinceArr = doc.value("since", nlohmann::json::array());
  size_t n = svcs.size();
  out.svcs.resize(n);

  /* ---- 历史序列：hist 是 [槽位号, 每位一个字符]，按时间升序 ---- */
  std::vector<std::pair<int64_t, std::string>> hist;
  if (doc.contains("hist") && doc["hist"].is_array()) {
    for (auto& h : doc["hist"]) {
      if (!h.is_array() || h.size() < 2) continue;
      if (!h[0].is_number() || !h[1].is_string()) continue;
      hist.push_back({(int64_t)h[0].get<long long>(), h[1].get<std::string>()});
    }
    std::sort(hist.begin(), hist.end(),
              [](const std::pair<int64_t, std::string>& a, const std::pair<int64_t, std::string>& b) {
                return a.first < b.first;
              });
  }

  int allDown = 0;
  for (size_t i = 0; i < n; i++) {
    auto& s = svcs[i];
    SvcStat& st = out.svcs[i];
    st.id = s.value("id", "");
    st.name = s.value("name", "");
    st.shortName = s.value("short", st.name);
    st.group = s.value("group", "");
    st.level = s.value("level", s.value("ok", false) ? "up" : "down");
    st.err = s.value("err", "");
    st.ms = s.value("ms", 0);
    st.ok = s.value("ok", false);
    st.badNow = (st.level == "down");

    int64_t sinceTs = 0;
    if (sinceArr.is_array() && i < sinceArr.size() && sinceArr[i].is_number())
      sinceTs = sinceArr[i].get<long long>();

    /* 24h：hist 全量（本身约 720 个点 = 24h）*/
    int64_t lastT = 0;
    bool lastBad = false;
    std::vector<std::pair<int64_t, int>> seq;   // 0 正常 1 慢 2 挂
    for (auto& h : hist) {
      if (i >= h.second.size()) continue;
      int64_t t = h.first * kSlotMs;
      if (sinceTs && t < sinceTs) continue;
      char ch = h.second[i];
      if (ch != '0' && ch != '1' && ch != '2') continue;
      int v = ch == '0' ? 2 : (ch == '2' ? 1 : 0);
      seq.push_back({t, v});
      lastT = t;
      lastBad = (v == 2);
      if (v == 2) {
        st.h24down++;
      } else {
        st.h24up++;
        if (v == 1) st.h24slow++;
      }
    }
    st.h24n = st.h24up + st.h24down;
    st.h24pct = pctOf(st.h24up, st.h24n);

    for (auto& x : seq) {
      if (x.second == 2) st.badCount++;
      if (x.second != 2) st.lastUpTs = x.first;
    }
    if (lastBad && !seq.empty()) {
      size_t k = seq.size();
      while (k > 0 && seq[k - 1].second == 2) k--;
      st.sinceTs = k < seq.size() ? seq[k].first : 0;
      if (k == 0) st.sinceTs = seq.front().first;
    }
    (void)lastT;

    /* 故障段：连续异常，空档 ≤ kMergeGapMs 合并；新的在前 */
    std::vector<Incident> segs;
    Incident cur;
    bool open = false;
    for (auto& x : seq) {
      if (x.second == 2) {
        if (!open) { cur = Incident{x.first, x.first}; open = true; }
        else cur.to = x.first;
      } else if (open) {
        segs.push_back(cur);
        open = false;
      }
    }
    if (open) segs.push_back(cur);
    std::vector<Incident> merged;
    for (auto& sg : segs) {
      if (!merged.empty() && sg.from - merged.back().to <= kMergeGapMs) {
        merged.back().to = sg.to;
      } else {
        merged.push_back(sg);
      }
    }
    std::reverse(merged.begin(), merged.end());
    st.incidents = merged;

    if (st.level == "down") allDown++;
  }

  out.level = allDown == 0 ? "up" : (allDown == (int)n ? "down" : "partial");

  /* ---- 近 7 天 ---- */
  std::map<std::string, nlohmann::json> daysMap;
  if (doc.contains("days") && doc["days"].is_object())
    for (auto it = doc["days"].begin(); it != doc["days"].end(); ++it)
      daysMap[it.key()] = it.value();
  std::vector<std::string> keys;
  for (auto& kv : daysMap) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  if (keys.size() > 7) keys.erase(keys.begin(), keys.end() - 7);
  out.days = keys;
  out.week.assign(n, std::vector<double>(keys.size(), -1));
  out.weekFailed.assign(n, std::vector<long long>(keys.size(), 0));

  for (size_t d = 0; d < keys.size(); d++) {
    auto& day = daysMap[keys[d]];
    for (size_t i = 0; i < n; i++) {
      long long cnt = 0;
      long long up = 0;
      if (day.contains("cnt") && day["cnt"].is_array() && i < day["cnt"].size() &&
          day["cnt"][i].is_number())
        cnt = day["cnt"][i].get<long long>();
      else
        cnt = day.value("n", 0);
      if (day.contains("ok") && day["ok"].is_array() && i < day["ok"].size() &&
          day["ok"][i].is_number())
        up = day["ok"][i].get<long long>();
      out.weekFailed[i][d] = cnt > up ? cnt - up : 0;
      out.week[i][d] = cnt >= 5 ? pctOf(up, cnt) : -1;
    }
  }

  /* 7 天可用率 = 各天合计 */
  for (size_t i = 0; i < n; i++) {
    long long up = 0, cnt = 0;
    for (size_t d = 0; d < keys.size(); d++) {
      if (out.week[i][d] < 0) continue;
      auto& day = daysMap[keys[d]];
      long long c = 0, u = 0;
      if (day.contains("cnt") && day["cnt"].is_array() && i < day["cnt"].size() &&
          day["cnt"][i].is_number())
        c = day["cnt"][i].get<long long>();
      else
        c = day.value("n", 0);
      if (day.contains("ok") && day["ok"].is_array() && i < day["ok"].size() &&
          day["ok"][i].is_number())
        u = day["ok"][i].get<long long>();
      cnt += c;
      up += u;
    }
    out.svcs[i].d7n = cnt;
    out.svcs[i].d7pct = cnt >= 5 ? pctOf(up, cnt) : -1;
  }
}

bool loadStatusSummary(Ctx& c, StatusSummary& out, std::string& err) {
  nlohmann::json j;
  if (!grpStatus(c, j, err)) return false;
  computeStatus(j, out);
  if (!out.ok) {
    err = out.error.empty() ? "状态数据不完整" : out.error;
    return false;
  }
  return true;
}

}  // namespace bn
