// statusview.h — 状态页数据的计算与摘要（CLI / TTY 共用）
//
// 数据来自 https://grp.blacknet.cc.cd/api/status（status-probe 每 2 分钟一次）：
//   services[] 当前状态；hist[] [槽位号, 每位一个 0/1/2 的字符串]；days{} 按天聚合；since[] 起点
// 本文件只做计算，不做渲染，CLI 与 TTY 各自排版。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "api.h"
#include "third_party/json.hpp"

namespace bn {

constexpr int64_t kSlotMs = 120000;        // 探测间隔
constexpr int64_t kMergeGapMs = 600000;    // 相邻故障段空档 ≤ 5 个探测点就合并（与状态页一致）

struct Incident {
  int64_t from = 0;   // ms
  int64_t to = 0;     // ms
};

struct SvcStat {
  std::string id;
  std::string name;
  std::string shortName;
  std::string group;
  std::string level;   // up / slow / down
  std::string err;
  int ms = 0;
  bool ok = false;

  int h24up = 0, h24n = 0, h24slow = 0, h24down = 0;
  double h24pct = -1;      // -1 = 无样本
  double d7pct = -1;
  long long d7n = 0;

  bool badNow = false;     // 最近一次探测是否异常
  int64_t sinceTs = 0;     // 当前这轮连续异常的起点（badNow 时有效）
  int64_t lastUpTs = 0;    // 最近一次成功
  int badCount = 0;
  std::vector<Incident> incidents;   // 新的在前，已按空档合并
};

struct StatusSummary {
  bool ok = false;
  std::string error;
  int64_t updatedMs = 0;
  int intervalSec = 120;
  std::string source;
  std::string level;                 // 全站：up / slow / down
  std::vector<SvcStat> svcs;
  std::vector<std::string> days;     // 最近 7 天（升序，YYYY-MM-DD）
  std::vector<std::vector<double>> week;      // [svc][day] 可用率，-1 = 无数据
  std::vector<std::vector<long long>> weekFailed;  // [svc][day] 失败次数
};

/* 取数 + 计算。失败时 summary.ok=false 且 error 有值 */
bool loadStatusSummary(Ctx& c, StatusSummary& out, std::string& err);
/* 纯计算（数据已拿到时用，TTY 复用） */
void computeStatus(const nlohmann::json& doc, StatusSummary& out);

/* 人读时间：本地时区 "11-14 13:20"；0 返回 "—" */
std::string fmtLocalMs(int64_t ms, bool withDate = true);
/* 时长：3 分 / 2 小时 05 分 / 1 天 3 小时 */
std::string fmtDuration(int64_t ms);
/* 可用率：-1 → "—"，否则 "99.9%" */
std::string fmtPct(double v);

}  // namespace bn
