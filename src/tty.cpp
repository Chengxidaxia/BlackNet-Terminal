// tty.cpp — bn 的全屏界面（BlackNet Terminal）
//
// 只面向键盘：所有操作都能用方向键 + 字母键完成，程序不读取、也不响应鼠标事件。
// 布局：标签栏 / 内容区 / 消息行 / 键位提示行。
// 所有网络请求都在后台线程里跑，界面立刻响应按键；线程只写 stage_，
// 主线程在 poll() 里一次性搬到界面状态，避免渲染时读到半成品。
#include "tty.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "api.h"
#include "bookfile.h"
#include "config.h"
#include "posts.h"
#include "statusview.h"
#include "third_party/json.hpp"
#include "ui.h"
#include "util.h"
#include "version.h"

namespace bn {
namespace {

/* ================= 颜色 ================= */
const WORD A_NORM = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
const WORD A_DIM = FOREGROUND_INTENSITY;
const WORD A_ACC = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
const WORD A_OK = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
const WORD A_WARN = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
const WORD A_ERR = FOREGROUND_RED | FOREGROUND_INTENSITY;
const WORD A_SEL =
    BACKGROUND_BLUE | BACKGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
const WORD A_TAB = BACKGROUND_GREEN | BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN |
                   FOREGROUND_BLUE | FOREGROUND_INTENSITY;

/* ================= 字符宽度 ================= */
int g_wideW = 2;   // 一个全角字符占几格，init() 里实测

int wcWidth(wchar_t c) {
  if (c == 0 || c < 32) return 0;
  if (c >= 0xD800 && c <= 0xDBFF) return g_wideW;
  if (c >= 0xDC00 && c <= 0xDFFF) return 0;
  return isWideChar((uint32_t)c) ? g_wideW : 1;
}

/* 用控制台自己的游标推进量测出全角宽度：写一个「中」看光标走了几格 */
int measureWide(HANDLE out) {
  CONSOLE_SCREEN_BUFFER_INFO ci;
  if (!GetConsoleScreenBufferInfo(out, &ci)) return 2;
  COORD save = ci.dwCursorPosition;
  COORD probe{0, 0};   // 必须在顶行：写到最后一行会让控制台滚屏
  DWORD n = 0;
  SetConsoleCursorPosition(out, probe);
  WriteConsoleW(out, L"中", 1, &n, nullptr);
  int w = 2;
  if (GetConsoleScreenBufferInfo(out, &ci)) w = ci.dwCursorPosition.X - probe.X;
  SetConsoleCursorPosition(out, save);
  return (w == 1 || w == 2) ? w : 2;
}
/* 向终端本身要真实可见尺寸。
   ConPTY 下 GetConsoleScreenBufferInfo 给的是 conhost 自己的缓冲区尺寸，可能和终端可见列数不一致。
   这里用「光标移到极限 + 位置报告（CPR）」问一次：ESC[9999;9999H 然后 ESC[6n，终端回 ESC[行;列R。
   问不到就返回 false，退回老办法。 */
bool queryTermSize(HANDLE hin, HANDLE hout, int& cols, int& rows) {
  DWORD oldIn = 0, oldOut = 0;
  if (!GetConsoleMode(hin, &oldIn) || !GetConsoleMode(hout, &oldOut)) return false;
  SetConsoleMode(hin, (oldIn | ENABLE_VIRTUAL_TERMINAL_INPUT) & ~ENABLE_PROCESSED_INPUT);
  if (!SetConsoleMode(hout, oldOut | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    SetConsoleMode(hin, oldIn);
    return false;
  }
  const char req[] = "\x1b[s\x1b[9999;9999H\x1b[6n\x1b[u";
  DWORD n = 0;
  WriteFile(hout, req, sizeof(req) - 1, &n, nullptr);

  bool ok = false;
  std::string buf;
  DWORD t0 = GetTickCount();
  while (GetTickCount() - t0 < 300) {
    DWORD avail = 0;
    if (!GetNumberOfConsoleInputEvents(hin, &avail) || avail == 0) {
      Sleep(5);
      continue;
    }
    INPUT_RECORD rec;
    DWORD got = 0;
    if (!ReadConsoleInputW(hin, &rec, 1, &got) || got == 0) break;
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
    wchar_t c = rec.Event.KeyEvent.uChar.UnicodeChar;
    if (c >= 32 && c < 128) buf += (char)c;
    size_t p = buf.find("[");
    while (p != std::string::npos) {
      int r = 0, cc = 0;
      if (sscanf_s(buf.c_str() + p, "[%d;%dR", &r, &cc) == 2 && r > 0 && cc > 0) {
        rows = r;
        cols = cc;
        ok = true;
        break;
      }
      p = buf.find("[", p + 1);
    }
    if (ok) break;
    if (buf.size() > 96) buf.erase(0, 48);
  }
  SetConsoleMode(hin, oldIn);
  SetConsoleMode(hout, oldOut);
  return ok;
}

/* 让「窗口」和「缓冲区」是同一个矩形。
   不对齐的话，用户把窗口拉窄/拉矮之后控制台会横向或纵向滚动，srWindow 的 Left/Top 不再是 0，
   整个界面就整体偏移、右边被切掉（1.0.0 的截断就是这样来的）。 */
bool alignBuffer(HANDLE out, COORD want) {
  if (want.X < 20) want.X = 20;
  if (want.Y < 8) want.Y = 8;
  CONSOLE_SCREEN_BUFFER_INFO ci;
  if (!GetConsoleScreenBufferInfo(out, &ci)) return false;
  if (ci.dwSize.X == want.X && ci.dwSize.Y == want.Y && ci.srWindow.Left == 0 &&
      ci.srWindow.Top == 0)
    return true;
  SMALL_RECT one{0, 0, 0, 0};
  SetConsoleWindowInfo(out, TRUE, &one);          // 先把窗口缩到 1x1，缓冲区才允许缩小
  if (!SetConsoleScreenBufferSize(out, want)) {   // 有的终端不让改，退回原样
    SMALL_RECT back{0, 0, (SHORT)std::min<int>(ci.dwSize.X - 1, ci.srWindow.Right - ci.srWindow.Left),
                    (SHORT)std::min<int>(ci.dwSize.Y - 1, ci.srWindow.Bottom - ci.srWindow.Top)};
    SetConsoleWindowInfo(out, TRUE, &back);
    return false;
  }
  SMALL_RECT full{0, 0, (SHORT)(want.X - 1), (SHORT)(want.Y - 1)};
  return SetConsoleWindowInfo(out, TRUE, &full);
}

int wstrWidth(const std::wstring& s) {
  int w = 0;
  for (wchar_t c : s) w += wcWidth(c);
  return w;
}
std::wstring repeatW(wchar_t c, int n) { return std::wstring((size_t)std::max(0, n), c); }
std::wstring truncateTo(const std::wstring& s, int width);
std::wstring padTo(const std::wstring& s, int width) {
  int w = wstrWidth(s);
  if (w > width) return truncateTo(s, width);
  return s + repeatW(L' ', width - w);
}
std::wstring truncateTo(const std::wstring& s, int width) {
  std::wstring out;
  int x = 0;
  for (wchar_t c : s) {
    int cw = wcWidth(c);
    if (x + cw > width) { out += L'…'; break; }
    out += c;
    x += cw;
  }
  return out;
}
std::wstring WW(const std::string& s) { return utf8ToWide(s); }

/* 按显示宽度折行，保留空行 */
std::vector<std::wstring> wrapText(const std::string& utf8, int width) {
  std::vector<std::wstring> out;
  if (width < 8) width = 8;
  std::wstring cur;
  int cw = 0;
  for (wchar_t c : utf8ToWide(utf8)) {
    if (c == L'\r') continue;
    if (c == L'\n') {
      out.push_back(cur);
      cur.clear();
      cw = 0;
      continue;
    }
    if (c == L'\t') c = L' ';
    int w = wcWidth(c);
    if (cw + w > width) {
      out.push_back(cur);
      cur.clear();
      cw = 0;
      if (c == L' ') continue;
    }
    cur += c;
    cw += w;
  }
  out.push_back(cur);
  return out;
}

/* ================= 画布 ================= */
/* 把 WORD 颜色属性翻成 ANSI SGR：现代终端（Windows Terminal / conhost 开了 VT）都认。 */
std::string sgrOf(WORD a) {
  int fgbits = a & 0x0F, bgbits = (a >> 4) & 0x0F;
  int f = 0, b = 0;
  if (fgbits & FOREGROUND_RED) f |= 1;
  if (fgbits & FOREGROUND_GREEN) f |= 2;
  if (fgbits & FOREGROUND_BLUE) f |= 4;
  if (bgbits & BACKGROUND_RED) b |= 1;
  if (bgbits & BACKGROUND_GREEN) b |= 2;
  if (bgbits & BACKGROUND_BLUE) b |= 4;
  int fg = (fgbits & FOREGROUND_INTENSITY) ? 90 + f : 30 + f;
  int bg = (bgbits & BACKGROUND_INTENSITY) ? 100 + b : 40 + b;
  char buf[24];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dm", fg, bg);
  return std::string(buf);
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

class Canvas {
 public:
  void begin(HANDLE out, const CONSOLE_SCREEN_BUFFER_INFO& ci, int maxW, int maxH, bool vt,
             bool clear = false) {
    out_ = out;
    vt_ = vt;
    clear_ = clear;
    origin_ = COORD{ci.srWindow.Left, ci.srWindow.Top};
    W_ = ci.srWindow.Right - ci.srWindow.Left + 1;
    H_ = ci.srWindow.Bottom - ci.srWindow.Top + 1;
    // 终端自报的可见尺寸比 conhost 的更可信：宁可少画，也别画到屏幕外面去
    if (maxW > 0 && maxW < W_) W_ = maxW;
    if (maxH > 0 && maxH < H_) H_ = maxH;
    if (W_ < 20) W_ = 20;
    if (H_ < 8) H_ = 8;
    buf_.assign((size_t)W_ * H_, CHAR_INFO{L' ', A_NORM});
  }
  int w() const { return W_; }
  int h() const { return H_; }
  void clear(WORD attr = A_NORM) {
    for (auto& c : buf_) c = CHAR_INFO{L' ', attr};
  }
  void put(int x, int y, const std::wstring& s, WORD attr, int limit = -1) {
    if (y < 0 || y >= H_) return;
    int end = (limit < 0) ? W_ : std::min(W_, limit);
    size_t i = 0;
    while (i < s.size()) {
      wchar_t ch = s[i];
      if (ch == L'\t') ch = L' ';
      bool surrogate = (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < s.size() &&
                        s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF);
      int cw = surrogate ? g_wideW : wcWidth(ch);
      if (cw == 0) { i++; continue; }
      if (x + cw > end) break;
      if (x < 0) { x += cw; i += surrogate ? 2 : 1; continue; }
      buf_[(size_t)y * W_ + x] = CHAR_INFO{ch, attr};
      if (surrogate && x + 1 < W_)
        buf_[(size_t)y * W_ + x + 1] = CHAR_INFO{s[i + 1], attr};
      else if (cw == 2 && x + 1 < W_)
        buf_[(size_t)y * W_ + x + 1] = CHAR_INFO{0, attr};
      x += cw;
      i += surrogate ? 2 : 1;
    }
  }
  void fill(int x, int y, int n, wchar_t ch, WORD attr) {
    if (y < 0 || y >= H_) return;
    for (int i = 0; i < n && x + i < W_; i++) buf_[(size_t)y * W_ + x + i] = CHAR_INFO{ch, attr};
  }
  void flush() {
    if (vt_) {
      flushVt();
      return;
    }
    COORD size{(SHORT)W_, (SHORT)H_};
    COORD pos{0, 0};
    SMALL_RECT rc{origin_.X, origin_.Y, (SHORT)(origin_.X + W_ - 1),
                  (SHORT)(origin_.Y + H_ - 1)};
    WriteConsoleOutputW(out_, buf_.data(), size, pos, &rc);
  }

 private:
  /* 一帧整屏写成 ANSI：光标定位到每行行首、按需换色、行尾清干净。
     这样不依赖「控制台缓冲区坐标」，也就不会因为窗口变化把内容推到历史里。 */
  void flushVt() {
    std::string out;
    out.reserve((size_t)W_ * 48 + 256);
    out += "\x1b[?25l";   // 藏光标
    if (clear_) out += "\x1b[2J";   // 尺寸刚变：和整帧一起写，避免「先擦白再画」闪一下
    WORD cur = 0xFFFF;
    for (int y = 0; y < H_; y++) {
      char cup[24];
      snprintf(cup, sizeof(cup), "\x1b[%d;1H", y + 1);
      out += cup;
      cur = 0xFFFF;
      for (int x = 0; x < W_; x++) {
        const CHAR_INFO& c = buf_[(size_t)y * W_ + x];
        wchar_t ch = c.Char.UnicodeChar;
        if (ch == 0) continue;                    // 双宽字符的占位格
        if (c.Attributes != cur) {
          out += "\x1b[0m";
          out += sgrOf(c.Attributes);
          cur = c.Attributes;
        }
        if (ch >= 0xD800 && ch <= 0xDBFF && x + 1 < W_) {
          wchar_t lo = buf_[(size_t)y * W_ + x + 1].Char.UnicodeChar;
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            appendUtf8(out, 0x10000u + ((uint32_t)(ch - 0xD800) << 10) +
                                 (uint32_t)(lo - 0xDC00));
            x++;
            continue;
          }
        }
        appendUtf8(out, (uint32_t)ch);
      }
      out += "\x1b[K";
    }
    out += "\x1b[0m";
    DWORD n = 0;
    WriteFile(out_, out.data(), (DWORD)out.size(), &n, nullptr);
  }

  HANDLE out_ = INVALID_HANDLE_VALUE;
  int W_ = 0, H_ = 0;
  bool vt_ = false;
  bool clear_ = false;
  COORD origin_{0, 0};
  std::vector<CHAR_INFO> buf_;
};

struct Row {
  std::wstring s;
  WORD a = A_NORM;
  bool sel = false;
};

/* ================= 数据 ================= */
struct PostItem {
  int number = 0;
  std::string title, cat, author, created, info;
  int upvotes = 0;
  bool pinned = false;
};

struct BookItem {
  std::string id, title, author, status, visibility, updated;
  int chapters = 0, words = 0;
};

struct ChapterItem {
  std::string title, content;
};

/* 后台线程的产出：只在 poll() 里被搬进界面状态 */
struct Staged {
  int kind = 0;   // 1 grp 2 detail 3 books 4 chapters 5 status
  std::string err;
  std::string msg;
  WORD msgAttr = A_DIM;
  int reloadDetail = 0;   // 评论成功后要重载的编号

  std::vector<PostItem> posts;
  std::vector<BookItem> books;
  std::vector<ChapterItem> chapters;
  std::string chapTitle;
  std::string bookId;

  bool inDetail = false;
  int detailNum = 0;
  std::string detailTitle, detailMeta, detailBody, detailId;
  std::vector<std::pair<std::string, std::string>> comments;

  StatusSummary st;
  bool hasStatus = false;
};

enum View { V_GRP = 0, V_BOOK = 1, V_STATUS = 2, V_HELP = 3 };
enum EditKind { E_NONE = 0, E_SEARCH_GRP, E_SEARCH_BOOK, E_COMMENT };

/* ================= 应用 ================= */
class App {
 public:
  int run();
  /* 无窗口模式：不碰控制台，只把各页排版成文本，供自检/回归用 */
  void headless(int w, int h);
  bool loadAllHeadless();
  std::vector<std::wstring> renderText();

 private:
  bool init();
  void shutdown();
  void loop();
  void draw();
  void handleKey(const KEY_EVENT_RECORD& k);

  void kick(std::function<void()> fn, const char* what);
  void poll();
  void applyStage();
  bool checkSize();     // 窗口/缓冲区变了就重新对齐并返回 true
  Ctx ctx() const;

  void loadGrp();
  void loadDetail(int number);
  void loadBooks();
  void loadChapters(int idx);
  void loadStatus();

  std::vector<Row> buildRows(int width);
  std::vector<Row> rowsGrp(int width);
  std::vector<Row> rowsDetail(int width);
  std::vector<Row> rowsBooks(int width);
  std::vector<Row> rowsChapters(int width);
  std::vector<Row> rowsChapter(int width);
  std::vector<Row> rowsStatus(int width);
  std::vector<Row> rowsHelp(int width);

  int listCount() const;
  int contentHeight() const;
  void clampSel();
  void setMsg(const std::string& s, WORD a = A_NORM);
  void refreshCurrent();

  HANDLE hin_ = INVALID_HANDLE_VALUE;
  HANDLE hout_ = INVALID_HANDLE_VALUE;
  DWORD oldInMode_ = 0;
  CONSOLE_CURSOR_INFO oldCursor_{};
  bool ready_ = false;

  Config cfg_;
  std::string me_;

  int view_ = V_GRP;
  int sel_ = 0;
  int scroll_ = 0;
  bool dirty_ = true;

  std::vector<PostItem> posts_;
  std::string catFilter_, sortKind_ = "default", query_;
  bool asc_ = false;

  bool inDetail_ = false;
  int detailNum_ = 0;
  std::string detailTitle_, detailMeta_, detailBody_, detailId_;
  std::vector<std::pair<std::string, std::string>> comments_;

  std::vector<BookItem> books_;
  std::string bookFilter_;
  bool inChapters_ = false;
  bool inChapter_ = false;
  int bookIdx_ = -1;
  int chapIdx_ = -1;
  std::vector<ChapterItem> chapters_;
  std::string chapTitle_;

  StatusSummary st_;

  Staged stage_;
  std::future<void> fut_;
  bool busy_ = false;
  std::string busyWho_;

  EditKind edit_ = E_NONE;
  std::string editBuf_;
  std::string msg_;
  WORD msgAttr_ = A_NORM;

  bool headless_ = false;
  int fakeW_ = 120, fakeH_ = 40;
  COORD oldBuf_{0, 0};        // 进界面前的缓冲区尺寸，退出时还原
  bool aligned_ = false;      // 成功把窗口与缓冲区对齐过
  int lastW_ = 0, lastH_ = 0; // 上一帧的可见尺寸，用来感知窗口变化
  int termW_ = 0, termH_ = 0; // 终端自报的可见尺寸（VT 查询），0 = 没问到
  bool vtOk_ = false;         // 能用 VT 序列画（现代终端）：走备用屏，不碰滚动历史
  bool sizeChanged_ = false;  // 上一帧之后窗口尺寸变过，这一帧要先清屏
};

Ctx App::ctx() const {
  Ctx c;
  c.cfg = cfg_;
  c.token = resolveToken(cfg_);
  return c;
}

void App::setMsg(const std::string& s, WORD a) {
  msg_ = s;
  msgAttr_ = a;
}

int App::contentHeight() const {
  if (headless_) return std::max(3, fakeH_ - 5);
  CONSOLE_SCREEN_BUFFER_INFO ci;
  if (!GetConsoleScreenBufferInfo(hout_, &ci)) return 20;
  int h = (ci.srWindow.Bottom - ci.srWindow.Top + 1) - 5;
  return std::max(3, h);
}

int App::listCount() const {
  if (view_ == V_GRP) return inDetail_ ? 0 : (int)posts_.size();
  if (view_ == V_BOOK) {
    if (inChapter_) return 0;
    return inChapters_ ? (int)chapters_.size() : (int)books_.size();
  }
  return 0;
}

void App::clampSel() {
  int n = listCount();
  if (n <= 0) return;
  if (sel_ < 0) sel_ = 0;
  if (sel_ >= n) sel_ = n - 1;
  int h = contentHeight();
  if (sel_ < scroll_) scroll_ = sel_;
  if (sel_ >= scroll_ + h) scroll_ = sel_ - h + 1;
  if (scroll_ < 0) scroll_ = 0;
}

/* ================= 生命周期 ================= */
bool App::init() {
  hout_ = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (hout_ == INVALID_HANDLE_VALUE || !GetConsoleMode(hout_, &mode)) return false;

  hin_ = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                     OPEN_EXISTING, 0, nullptr);
  if (hin_ == INVALID_HANDLE_VALUE) hin_ = GetStdHandle(STD_INPUT_HANDLE);
  GetConsoleMode(hin_, &oldInMode_);
  // 只要按键与窗口尺寸事件：不排队鼠标事件，界面天然不响应点击
  SetConsoleMode(hin_, ENABLE_WINDOW_INPUT);

  GetConsoleCursorInfo(hout_, &oldCursor_);
  CONSOLE_CURSOR_INFO ci = oldCursor_;
  ci.bVisible = FALSE;
  SetConsoleCursorInfo(hout_, &ci);

  SetConsoleTitleW(L"bn · BlackNet Terminal");

  CONSOLE_SCREEN_BUFFER_INFO ci0;
  if (GetConsoleScreenBufferInfo(hout_, &ci0)) {
    oldBuf_ = ci0.dwSize;
    lastW_ = ci0.srWindow.Right - ci0.srWindow.Left + 1;
    lastH_ = ci0.srWindow.Bottom - ci0.srWindow.Top + 1;
  }
  // 能开 VT 就进备用屏：界面画在另一个屏上，窗口拉大拉小都不会把内容推进历史
  DWORD om = 0;
  if (GetConsoleMode(hout_, &om) &&
      SetConsoleMode(hout_, om | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    vtOk_ = true;
    const char enter[] = "\x1b[?1049h\x1b[H\x1b[2J\x1b[?25l";
    DWORD n = 0;
    WriteFile(hout_, enter, sizeof(enter) - 1, &n, nullptr);
  } else {
    CONSOLE_SCREEN_BUFFER_INFO ci1;
    if (GetConsoleScreenBufferInfo(hout_, &ci1)) {
      COORD vp{(SHORT)(ci1.srWindow.Right - ci1.srWindow.Left + 1),
               (SHORT)(ci1.srWindow.Bottom - ci1.srWindow.Top + 1)};
      aligned_ = alignBuffer(hout_, vp);
    }
  }
  if (queryTermSize(hin_, hout_, termW_, termH_)) {
    if (termW_ < 20 || termH_ < 8) { termW_ = 0; termH_ = 0; }
  } else {
    termW_ = 0;
    termH_ = 0;
  }
  g_wideW = measureWide(hout_);
  cfg_ = loadConfig();
  me_ = cfg_.login;
  ready_ = true;
  return true;
}

void App::shutdown() {
  if (!ready_) return;
  if (vtOk_) {
    const char leave[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
    DWORD n = 0;
    WriteFile(hout_, leave, sizeof(leave) - 1, &n, nullptr);
    vtOk_ = false;
  }
  CONSOLE_CURSOR_INFO ci = oldCursor_;
  SetConsoleCursorInfo(hout_, &ci);
  SetConsoleMode(hin_, oldInMode_);
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(hout_, &info)) {
    COORD home{info.srWindow.Left, info.srWindow.Top};
    DWORD n = 0;
    FillConsoleOutputCharacterW(hout_, L' ', (DWORD)info.dwSize.X * info.dwSize.Y, home, &n);
    SetConsoleCursorPosition(hout_, home);
    // 把缓冲区尺寸还回去，用户退出后滚回历史才对得上
    if (aligned_ && oldBuf_.X > 0 && oldBuf_.Y > 0 &&
        (oldBuf_.X != info.dwSize.X || oldBuf_.Y != info.dwSize.Y)) {
      COORD cur{info.dwSize.X, info.dwSize.Y};
      COORD back{(SHORT)std::max<int>(oldBuf_.X, cur.X), (SHORT)std::max<int>(oldBuf_.Y, cur.Y)};
      SMALL_RECT one{0, 0, 0, 0};
      SetConsoleWindowInfo(hout_, TRUE, &one);
      SetConsoleScreenBufferSize(hout_, back);
      SMALL_RECT full{0, 0, (SHORT)(info.srWindow.Right - info.srWindow.Left),
                      (SHORT)(info.srWindow.Bottom - info.srWindow.Top)};
      SetConsoleWindowInfo(hout_, TRUE, &full);
    }
  }
  ready_ = false;
}

/* ================= 取数 ================= */
void App::kick(std::function<void()> fn, const char* what) {
  if (headless_) {
    stage_ = Staged();
    try {
      fn();
    } catch (const std::exception& e) {
      stage_.err = std::string("内部错误：") + e.what();
    }
    applyStage();
    return;
  }
  if (busy_) {
    setMsg("上一件事还没做完（" + busyWho_ + "），等一下", A_WARN);
    dirty_ = true;
    return;
  }
  busy_ = true;
  busyWho_ = what;
  stage_ = Staged();
  dirty_ = true;
  fut_ = std::async(std::launch::async, [this, fn]() {
    try {
      fn();
    } catch (const std::exception& e) {
      stage_.err = std::string("内部错误：") + e.what();
      stage_.kind = 0;
    } catch (...) {
      stage_.err = "内部错误";
      stage_.kind = 0;
    }
  });
}

void App::poll() {
  if (!busy_ || !fut_.valid()) return;
  if (fut_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
  fut_.get();
  busy_ = false;
  applyStage();
}

void App::applyStage() {
  int kind = stage_.kind;
  if (!stage_.err.empty()) setMsg(stage_.err, A_ERR);

  switch (kind) {
    case 1:
      posts_ = std::move(stage_.posts);
      sel_ = 0;
      scroll_ = 0;
      if (stage_.err.empty() && !stage_.msg.empty()) setMsg(stage_.msg, stage_.msgAttr);
      break;
    case 2:
      if (stage_.err.empty()) {
        inDetail_ = true;
        detailNum_ = stage_.detailNum;
        detailTitle_ = stage_.detailTitle;
        detailMeta_ = stage_.detailMeta;
        detailBody_ = stage_.detailBody;
        detailId_ = stage_.detailId;
        comments_ = stage_.comments;
        scroll_ = 0;
      }
      break;
    case 3:
      books_ = std::move(stage_.books);
      sel_ = 0;
      scroll_ = 0;
      inChapters_ = false;
      inChapter_ = false;
      if (stage_.err.empty() && !stage_.msg.empty()) setMsg(stage_.msg, stage_.msgAttr);
      break;
    case 4:
      if (stage_.err.empty()) {
        chapters_ = std::move(stage_.chapters);
        chapTitle_ = stage_.chapTitle;
        bookIdx_ = -1;
        for (size_t i = 0; i < books_.size(); i++)
          if (books_[i].id == stage_.bookId) bookIdx_ = (int)i;
        inChapters_ = true;
        inChapter_ = false;
        sel_ = 0;
        scroll_ = 0;
        setMsg("《" + chapTitle_ + "》 " + std::to_string(chapters_.size()) + " 章", A_DIM);
      }
      break;
    case 5:
      if (stage_.err.empty() && stage_.hasStatus) {
        st_ = stage_.st;
        setMsg("状态更新于 " + fmtLocalMs(st_.updatedMs) + "，每 " +
                   std::to_string(st_.intervalSec / 60) + " 分钟探测",
               A_DIM);
      }
      break;
    default: break;
  }
  if (!stage_.msg.empty() && kind == 0 && stage_.err.empty()) setMsg(stage_.msg, stage_.msgAttr);
  if (stage_.reloadDetail > 0) {
    int n = stage_.reloadDetail;
    stage_.reloadDetail = 0;
    loadDetail(n);
  }
  clampSel();
  dirty_ = true;
}

void App::loadGrp() {
  std::string cat = catFilter_, sort = sortKind_, q = query_;
  bool asc = asc_;
  kick(
      [this, cat, sort, q, asc]() {
        Ctx c = ctx();
        std::vector<PostItem> out;
        long want = (sort == "likes") ? 100 : 60;
        std::string cursor, err;
        while ((long)out.size() < want) {
          nlohmann::json j;
          if (!grpList(c, 50, cursor, j, err)) {
            if (out.empty()) { stage_.err = err; return; }
            break;
          }
          for (auto& d : j.value("nodes", nlohmann::json::array())) {
            PostMeta m = parsePostBody(d.value("body", ""));
            PostItem it;
            it.number = d.value("number", 0);
            it.title = d.value("title", "");
            it.cat = m.hasCategory ? catName(m.category) : "";
            it.author = (d.contains("author") && d["author"].is_object())
                            ? d["author"].value("login", "")
                            : "";
            it.created = d.value("createdAt", "");
            it.info = m.info;
            it.upvotes = d.value("upvoteCount", 0);
            it.pinned = (m.hasCategory && m.category == 1);
            out.push_back(std::move(it));
          }
          if (!j.contains("pageInfo") || !j["pageInfo"].value("hasNextPage", false)) break;
          cursor = j["pageInfo"].value("endCursor", "");
          if (cursor.empty()) break;
        }

        if (!q.empty()) {
          std::vector<PostItem> hit;
          for (auto& it : out)
            if (icontains(it.title, q) || icontains(it.info, q)) hit.push_back(it);
          out.swap(hit);
        }
        if (!cat.empty()) {
          std::vector<PostItem> hit;
          for (auto& it : out)
            if (it.cat == cat) hit.push_back(it);
          out.swap(hit);
        }
        std::stable_sort(out.begin(), out.end(), [&](const PostItem& x, const PostItem& y) {
          if (sort == "default") {
            if (x.pinned != y.pinned) return x.pinned && !asc;
            return asc ? x.created < y.created : x.created > y.created;
          }
          if (sort == "likes") {
            if (x.upvotes != y.upvotes) return asc ? x.upvotes < y.upvotes : x.upvotes > y.upvotes;
            return x.created > y.created;
          }
          return asc ? x.created < y.created : x.created > y.created;
        });

        stage_.posts = std::move(out);
        stage_.kind = 1;
        stage_.msg = "主站 " + std::to_string(stage_.posts.size()) + " 篇" +
                     (q.empty() ? std::string() : "（搜索：" + q + "）") +
                     (cat.empty() ? std::string() : "（分类：" + cat + "）");
        stage_.msgAttr = A_DIM;
      },
      "主站列表");
}

void App::loadDetail(int number) {
  kick(
      [this, number]() {
        Ctx c = ctx();
        nlohmann::json j;
        std::string err;
        if (!grpShow(c, number, 40, j, err)) { stage_.err = err; return; }
        if (!j.contains("discussion") || j["discussion"].is_null()) {
          stage_.err = "没有这篇文章：#" + std::to_string(number);
          return;
        }
        auto d = j["discussion"];
        PostMeta m = parsePostBody(d.value("body", ""));
        stage_.detailNum = number;
        stage_.detailId = d.value("id", "");
        stage_.detailTitle = "#" + std::to_string(number) + " " + d.value("title", "");
        std::string author = (d.contains("author") && d["author"].is_object())
                                 ? d["author"].value("login", "")
                                 : "?";
        stage_.detailMeta = author + " · " + isoToLocal(d.value("createdAt", "")) +
                            (m.hasCategory ? " · " + std::string(catName(m.category)) : "") +
                            " · " + std::to_string(d.value("upvoteCount", 0)) + " 赞";
        stage_.detailBody = trim(m.rest);
        if (d.contains("comments") && d["comments"].is_object()) {
          for (auto& cm : d["comments"].value("nodes", nlohmann::json::array())) {
            std::string who = (cm.contains("author") && cm["author"].is_object())
                                  ? cm["author"].value("login", "?")
                                  : "?";
            stage_.comments.push_back({who, cm.value("body", "")});
          }
        }
        stage_.kind = 2;
      },
      "文章");
}

void App::loadBooks() {
  std::string q = bookFilter_;
  kick(
      [this, q]() {
        Ctx c = ctx();
        nlohmann::json idx;
        std::string err;
        if (!bookIndex(c, idx, err)) { stage_.err = err; return; }
        std::vector<BookItem> out;
        for (auto& b : idx.value("books", nlohmann::json::array())) {
          BookItem it;
          it.id = b.value("id", "");
          it.title = b.value("title", "");
          it.author = b.value("author", "");
          it.status = b.value("status", "ongoing");
          it.visibility = b.value("visibility", "public");
          it.updated = b.value("updatedAt", "");
          it.chapters = b.value("chapters", 0);
          it.words = b.value("words", 0);
          if (!q.empty() && !icontains(it.title + " " + it.author + " " + b.value("intro", ""), q))
            continue;
          out.push_back(std::move(it));
        }
        std::stable_sort(out.begin(), out.end(), [](const BookItem& x, const BookItem& y) {
          return x.updated > y.updated;
        });
        stage_.books = std::move(out);
        stage_.kind = 3;
        stage_.msg = "书阁 " + std::to_string(stage_.books.size()) + " 本" +
                     (q.empty() ? std::string() : "（搜索：" + q + "）");
        stage_.msgAttr = A_DIM;
      },
      "书阁");
}

void App::loadChapters(int idx) {
  if (idx < 0 || idx >= (int)books_.size()) return;
  BookItem b = books_[(size_t)idx];
  kick(
      [this, b]() {
        if (b.visibility == "private") {
          stage_.err = "《" + b.title + "》是私密书：命令行用 bn book show " + b.id +
                       " --password 口令 阅读";
          return;
        }
        Ctx c = ctx();
        std::string text, err;
        if (!bookFetch(c, b.id, b.id + ".book", text, err)) { stage_.err = err; return; }
        BookFile bf;
        if (!bookUnpack(text, siteKey(), bf, err)) { stage_.err = err; return; }
        for (auto& ch : bf.chapters)
          stage_.chapters.push_back({ch.value("title", ""), ch.value("content", "")});
        stage_.chapTitle = b.title;
        stage_.bookId = b.id;
        stage_.kind = 4;
      },
      "章节");
}

void App::loadStatus() {
  kick(
      [this]() {
        Ctx c = ctx();
        StatusSummary s;
        std::string err;
        if (!loadStatusSummary(c, s, err)) { stage_.err = err; return; }
        stage_.st = s;
        stage_.hasStatus = true;
        stage_.kind = 5;
      },
      "状态");
}

void App::refreshCurrent() {
  switch (view_) {
    case V_GRP:
      if (inDetail_) loadDetail(detailNum_);
      else loadGrp();
      break;
    case V_BOOK:
      if (inChapter_) { inChapter_ = false; inChapters_ = true; scroll_ = 0; }
      else if (inChapters_) { inChapters_ = false; }
      else loadBooks();
      break;
    case V_STATUS: loadStatus(); break;
    default: break;
  }
}

/* ================= 排版 ================= */
std::vector<Row> App::rowsGrp(int width) {
  std::vector<Row> out;
  if (posts_.empty()) {
    out.push_back({busy_ ? L"正在读取主站…" : L"（没有文章）", A_DIM, false});
    return out;
  }
  /* 列宽按可用宽度逐档收缩，而不是以某个宽度一刀切：
     先保标题（至少 24 格），再依次收掉点赞、日期、作者。这样拉窗口时
     是「一列一列地淡出」，不会整行排版突然跳一下。 */
  const int fixed = 5 + 6 + 1;              // 编号 + 分类 + 分隔
  const int authorW = 13, dateW = 6, upW = 3;
  int rest = width - fixed;
  bool showUp = rest - 24 >= 1 + authorW + 1 + dateW + 1 + upW;
  bool showDate = rest - 24 >= 1 + authorW + 1 + dateW;
  bool showAuthor = rest - 24 >= 1 + authorW;
  int tailW = 0;
  if (showUp) tailW += 1 + authorW + 1 + dateW + 1 + upW;
  else if (showDate) tailW += 1 + authorW + 1 + dateW;
  else if (showAuthor) tailW += 1 + authorW;
  int titleW = std::max(12, rest - tailW);

  for (size_t i = 0; i < posts_.size(); i++) {
    auto& p = posts_[i];
    std::wstring line = padTo(WW(std::to_string(p.number)), 5) +
                        padTo(WW(p.cat.empty() ? "—" : p.cat), 6) + L" ";
    std::wstring tail;
    if (showAuthor)
      tail += L" " + padTo(WW(p.author), authorW);
    if (showDate)
      tail += L" " + padTo(WW(p.created.size() >= 10 ? p.created.substr(5, 5) : p.created), dateW);
    if (showUp) tail += L" " + padTo(WW(std::to_string(p.upvotes)), upW);
    line += padTo(WW(p.title), titleW) + tail;
    out.push_back({truncateTo(line, width), p.pinned ? A_WARN : A_NORM, (int)i == sel_});
  }
  return out;
}

std::vector<Row> App::rowsDetail(int width) {
  std::vector<Row> out;
  out.push_back({truncateTo(WW(detailTitle_), width), A_ACC, false});
  for (auto& l : wrapText(detailMeta_, width)) out.push_back({l, A_DIM, false});
  out.push_back({L"", A_NORM, false});
  for (auto& l : wrapText(detailBody_, width)) out.push_back({l, A_NORM, false});
  if (!comments_.empty()) {
    out.push_back({L"", A_NORM, false});
    out.push_back({L"评论（" + std::to_wstring(comments_.size()) + L" 条）", A_ACC, false});
    for (auto& cm : comments_) {
      out.push_back({WW(cm.first), A_OK, false});
      for (auto& l : wrapText(cm.second, width - 2)) out.push_back({L"  " + l, A_NORM, false});
      out.push_back({L"", A_NORM, false});
    }
  }
  return out;
}

std::vector<Row> App::rowsBooks(int width) {
  std::vector<Row> out;
  if (books_.empty()) {
    out.push_back({busy_ ? L"正在读取书阁…" : L"（没有找到书）", A_DIM, false});
    return out;
  }
  /* 同样是逐档收缩：字数 → 作者收窄 → 章数，标题弹性吃掉剩余宽度 */
  int statusW = 6 + 4;                       // 完结/连载 + 公开/私密
  int authorW = width - 30 - 1 - 9 - 12 - statusW >= 0 ? 14 : 10;
  bool showWords = width >= authorW + 9 + 12 + statusW + 26;
  bool showChaps = width >= authorW + 9 + statusW + 20;
  int used = authorW + statusW + (showChaps ? 9 : 0) + (showWords ? 12 : 0);
  int titleW = std::max(16, width - used);

  for (size_t i = 0; i < books_.size(); i++) {
    auto& b = books_[i];
    std::wstring line = padTo(WW(b.title), titleW) +
                        padTo(WW(b.author), authorW);
    if (showChaps) line += padTo(WW(std::to_string(b.chapters) + " 章"), 9);
    if (showWords) line += padTo(WW(std::to_string(b.words) + " 字"), 12);
    line += padTo(WW(b.status == "done" ? "完结" : "连载"), 6) +
            WW(b.visibility == "private" ? "私密" : "公开");
    out.push_back({truncateTo(line, width), b.visibility == "private" ? A_WARN : A_NORM,
                   (int)i == sel_});
  }
  return out;
}

std::vector<Row> App::rowsChapters(int width) {
  std::vector<Row> out;
  out.push_back({truncateTo(L"《" + WW(chapTitle_) + L"》", width), A_ACC, false});
  out.push_back({L"", A_NORM, false});
  for (size_t i = 0; i < chapters_.size(); i++) {
    std::wstring line = padTo(WW(std::to_string(i + 1)), 4) + WW(chapters_[i].title);
    out.push_back({truncateTo(line, width), A_NORM, (int)i == sel_});
  }
  return out;
}

std::vector<Row> App::rowsChapter(int width) {
  std::vector<Row> out;
  if (chapIdx_ < 0 || chapIdx_ >= (int)chapters_.size()) return out;
  out.push_back({truncateTo(L"第 " + std::to_wstring(chapIdx_ + 1) + L" 章 · " +
                                WW(chapters_[(size_t)chapIdx_].title),
                            width),
                 A_ACC, false});
  out.push_back({L"", A_NORM, false});
  for (auto& l : wrapText(chapters_[(size_t)chapIdx_].content, width))
    out.push_back({l, A_NORM, false});
  return out;
}

std::vector<Row> App::rowsStatus(int width) {
  std::vector<Row> out;
  if (!st_.ok) {
    out.push_back({busy_ ? L"正在读取状态…" : L"（暂时读不到状态数据，按 r 重试）",
                   busy_ ? A_DIM : A_ERR, false});
    return out;
  }
  auto levelText = [](const std::string& lv) {
    if (lv == "up") return std::string("正常");
    if (lv == "slow") return std::string("偏慢");
    if (lv == "partial") return std::string("部分异常");
    return std::string("异常");
  };
  out.push_back({truncateTo(padTo(L"服务", 18) + padTo(L"状态", 9) + padTo(L"延迟", 8) +
                                padTo(L"24h", 8) + L"7天",
                            width),
                 A_DIM, false});
  for (auto& v : st_.svcs) {
    std::wstring line = padTo(WW(v.shortName), 18) + padTo(WW(levelText(v.level)), 9) +
                        padTo(WW(v.ms > 0 ? std::to_string(v.ms) + "ms" : "—"), 8) +
                        padTo(WW(fmtPct(v.h24pct)), 8) + WW(fmtPct(v.d7pct));
    WORD a = A_NORM;
    if (v.level == "down") a = A_ERR;
    else if (v.level == "slow") a = A_WARN;
    out.push_back({truncateTo(line, width), a, false});
  }

  out.push_back({L"", A_NORM, false});
  out.push_back({L"近期故障（近 24 小时）", A_ACC, false});
  int inc = 0;
  for (auto& v : st_.svcs) {
    int shown = 0;
    for (auto& x : v.incidents) {
      if (shown++ >= 3) break;
      inc++;
      std::wstring line = padTo(WW(v.shortName), 18) + WW(fmtLocalMs(x.from)) + L" → " +
                          WW(fmtLocalMs(x.to)) + L"  " + WW(fmtDuration(x.to - x.from));
      out.push_back({truncateTo(line, width), A_WARN, false});
    }
  }
  if (inc == 0) out.push_back({L"没有故障", A_OK, false});

  out.push_back({L"", A_NORM, false});
  out.push_back({L"近 7 天可用率", A_ACC, false});
  std::wstring head = padTo(L"服务", 18);
  for (auto& d : st_.days) head += padTo(WW(d.substr(5)), 7);
  out.push_back({truncateTo(head, width), A_DIM, false});
  for (size_t i = 0; i < st_.svcs.size(); i++) {
    std::wstring line = padTo(WW(st_.svcs[i].shortName), 18);
    for (size_t d = 0; d < st_.days.size(); d++) line += padTo(WW(fmtPct(st_.week[i][d])), 7);
    out.push_back({truncateTo(line, width), A_NORM, false});
  }
  return out;
}

std::vector<Row> App::rowsHelp(int width) {
  static const char* kLines[] = {
      "BlackNet Terminal —— 一个键盘就能走完全程",
      "",
      "全局",
      "  Tab / Shift+Tab   切换 主站 / 书阁 / 状态 / 帮助",
      "  1 2 3 4           直接跳到对应页面",
      "  r                 重新读取当前页",
      "  q / Ctrl+C        退出",
      "",
      "主站",
      "  ↑ ↓ / j k         选择        g / G         跳到首 / 尾",
      "  Enter             打开文章    Esc           返回列表",
      "  /                 搜索标题    c             切换分类",
      "  s                 切换排序（默认 / 最新 / 点赞）",
      "  a                 升序 / 降序",
      "",
      "文章",
      "  ↑ ↓ / PgUp PgDn   滚动        Home / End    首 / 尾",
      "  u                 点赞 / 取消",
      "  c                 写一条评论（Enter 发送，Esc 取消）",
      "",
      "书阁",
      "  ↑ ↓               选书        Enter         章节列表",
      "  Enter（章节里）   读这一章    Esc           返回上一级",
      "  /                 搜索书名 / 作者",
      "",
      "说明",
      "  界面只用键盘：不读取鼠标，也不响应点击。",
      "  网络请求都在后台线程，按键不会卡住。",
      "  私密书请用命令行：bn book show <书号> --password 口令",
  };
  std::vector<Row> out;
  int cw2 = headless_ ? fakeW_ : (termW_ > 0 ? termW_ : lastW_);
  int ch2 = headless_ ? fakeH_ : (termH_ > 0 ? termH_ : lastH_);
  for (auto& l : kLines) {
    std::string s = l;
    WORD a = A_NORM;
    bool isHead = !s.empty() && s[0] != ' ' && s.find("——") == std::string::npos;
    if (s.empty()) a = A_DIM;
    else if (isHead) a = A_ACC;
    out.push_back({truncateTo(WW(s), width), a, false});
  }
  if (cw2 > 0)
    out.push_back(
        {WW("  当前界面 " + std::to_string(cw2) + "×" + std::to_string(ch2) +
            " 格。汉字按 2 格排版；若字与字之间看着有缝，是终端字体没有等宽 CJK 字形"
            "（换成更纱黑体 / Maple Mono NF 之类会整）"),
         A_DIM, false});
  return out;
}

std::vector<Row> App::buildRows(int width) {
  if (view_ == V_GRP) return inDetail_ ? rowsDetail(width) : rowsGrp(width);
  if (view_ == V_BOOK) {
    if (inChapter_) return rowsChapter(width);
    if (inChapters_) return rowsChapters(width);
    return rowsBooks(width);
  }
  if (view_ == V_STATUS) return rowsStatus(width);
  return rowsHelp(width);
}

/* ================= 渲染 ================= */
void App::draw() {
  CONSOLE_SCREEN_BUFFER_INFO ci;
  if (!GetConsoleScreenBufferInfo(hout_, &ci)) return;
  Canvas cv;
  cv.begin(hout_, ci, termW_, termH_, vtOk_, sizeChanged_);
  const int W = cv.w(), H = cv.h();
  cv.clear();
  sizeChanged_ = false;

  /* 标签栏（留一行上边距：某些终端会把最顶行吃掉） */
  const int topY = 1;
  cv.put(0, topY, L" bn " + WW(kVersion) + L" ", A_ACC);
  int x = 4 + wstrWidth(WW(kVersion));
  static const wchar_t* tabs[] = {L"主站", L"书阁", L"状态", L"帮助"};
  for (int i = 0; i < 4; i++) {
    std::wstring item = (i == view_ ? L"▸" : L" ") + std::wstring(tabs[i]) + L"  ";
    cv.put(x, topY, item, i == view_ ? (A_NORM | BACKGROUND_INTENSITY) : A_DIM);
    x += wstrWidth(item);
  }
  std::wstring who = me_.empty() ? std::wstring(L"未登录 · 只读") : WW("@" + me_);
  cv.put(W - wstrWidth(who) - 2, topY, who, me_.empty() ? A_WARN : A_OK);
  cv.fill(0, topY + 1, W, L'─', A_DIM);

  const int contentY = topY + 2;
  const int contentH = std::max(3, H - 5);
  std::vector<Row> rows = buildRows(W - 2);
  int total = (int)rows.size();
  int maxScroll = std::max(0, total - contentH);
  if (scroll_ > maxScroll) scroll_ = maxScroll;
  if (scroll_ < 0) scroll_ = 0;

  for (int i = 0; i < contentH && scroll_ + i < total; i++) {
    Row& r = rows[(size_t)(scroll_ + i)];
    int y = contentY + i;
    if (r.sel) {
      cv.fill(1, y, W - 2, L' ', A_SEL);
      cv.put(1, y, r.s, A_SEL, W - 1);
    } else {
      cv.put(1, y, r.s, r.a, W - 1);
    }
  }

  if (total > contentH) {
    int barH = std::max(1, contentH * contentH / total);
    int barTop = contentY + (int)((long long)scroll_ * (contentH - barH) / std::max(1, maxScroll));
    for (int i = 0; i < contentH; i++) cv.put(W - 1, contentY + i, L"│", A_DIM);
    for (int i = 0; i < barH; i++) cv.put(W - 1, barTop + i, L"█", A_ACC);
  }

  cv.fill(0, H - 2, W, L'─', A_DIM);

  if (edit_ != E_NONE) {
    std::wstring label = edit_ == E_COMMENT ? L"评论： " : L"搜索： ";
    cv.put(1, H - 2, label + WW(editBuf_) + L"_", A_TAB, W - 1);
  } else {
    cv.put(1, H - 2, WW(msg_), msgAttr_, W - 1);
  }

  if (busy_) {
    cv.put(1, H - 1, L"⋯ 正在读取" + WW(busyWho_) + L"…", A_WARN, W - 1);
    return;
  }

  std::wstring keys;
  if (edit_ != E_NONE) {
    keys = L"Enter 确认   Esc 取消   Backspace 删除";
  } else if (view_ == V_GRP) {
    keys = inDetail_ ? L"↑↓ 滚动  u 点赞  c 评论  r 刷新  Esc 返回  Tab 切换  q 退出"
                     : L"↑↓ 选择  Enter 打开  / 搜索  c 分类  s 排序  a 升降  r 刷新  Tab 切换  q 退出";
  } else if (view_ == V_BOOK) {
    if (inChapter_) keys = L"↑↓ PgUp PgDn 滚动  Esc 返回  Tab 切换  q 退出";
    else if (inChapters_) keys = L"↑↓ 选择  Enter 阅读  Esc 返回  Tab 切换  q 退出";
    else keys = L"↑↓ 选书  Enter 打开  / 搜索  r 刷新  Tab 切换  q 退出";
  } else if (view_ == V_STATUS) {
    keys = L"↑↓ PgUp PgDn 滚动  r 刷新  Tab 切换  q 退出";
  } else {
    keys = L"Tab 切换  q 退出";
  }
  cv.put(1, H - 1, truncateTo(keys, W - 2), A_DIM, W - 1);
  cv.flush();

  if (const char* pf = getenv("BN_TUI_PROBE")) {
    static int done = 0;
    static int lastCW = -1, lastCH = -1;
    // 只在「可见尺寸变化」时落盘：用来验证拖窗口后是否按新尺寸重排
    if (W != lastCW || H != lastCH) {
      lastCW = W;
      lastCH = H;
      done++;
      std::vector<CHAR_INFO> all((size_t)W * H);
      COORD sz{(SHORT)W, (SHORT)H}, pz{0, 0};
      CONSOLE_SCREEN_BUFFER_INFO cb2;
      GetConsoleScreenBufferInfo(hout_, &cb2);
      SMALL_RECT rr{cb2.srWindow.Left, cb2.srWindow.Top, cb2.srWindow.Right, cb2.srWindow.Bottom};
      ReadConsoleOutputW(hout_, all.data(), sz, pz, &rr);
      std::string dump;
      char head[128];
      snprintf(head, sizeof(head),
               "frame=%d buffer=%dx%d win=(%d,%d)-(%d,%d) term=%dx%d canvas=%dx%d wide=%d\n",
               done, cb2.dwSize.X, cb2.dwSize.Y, cb2.srWindow.Left, cb2.srWindow.Top,
               cb2.srWindow.Right, cb2.srWindow.Bottom, termW_, termH_, W, H, g_wideW);
      dump += head;
      for (int y = 0; y < H; y++) {
        std::wstring line;
        for (int x = 0; x < W; x++) line += all[(size_t)y * W + x].Char.UnicodeChar;
        dump += utf8ToWideLossless(line) + "\n";
      }
      writeFile(pf, dump);
    }
  }
}

/* ================= 交互 ================= */
void App::handleKey(const KEY_EVENT_RECORD& k) {
  DWORD ctl = k.dwControlKeyState;
  bool ctrl = (ctl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
  if (ctrl && k.wVirtualKeyCode == 'C') {
    PostQuitMessage(0);
    return;
  }

  /* ---- 输入框 ---- */
  if (edit_ != E_NONE) {
    if (k.wVirtualKeyCode == VK_ESCAPE) {
      edit_ = E_NONE;
      editBuf_.clear();
      dirty_ = true;
      return;
    }
    if (k.wVirtualKeyCode == VK_RETURN) {
      EditKind kind = edit_;
      std::string text = trim(editBuf_);
      edit_ = E_NONE;
      editBuf_.clear();
      dirty_ = true;
      if (!text.empty()) {
        if (kind == E_SEARCH_GRP) {
          query_ = text;
          loadGrp();
        } else if (kind == E_SEARCH_BOOK) {
          bookFilter_ = text;
          loadBooks();
        } else if (kind == E_COMMENT) {
          std::string id = detailId_;
          int num = detailNum_;
          kick(
              [this, id, text, num]() {
                Ctx c = ctx();
                nlohmann::json out;
                std::string err;
                if (!grpComment(c, id, text, "", out, err)) { stage_.err = err; return; }
                stage_.msg = "评论已发送";
                stage_.msgAttr = A_OK;
                stage_.reloadDetail = num;
              },
              "发评论");
        }
      } else if (kind == E_SEARCH_GRP) {
        query_.clear();
        loadGrp();
      } else if (kind == E_SEARCH_BOOK) {
        bookFilter_.clear();
        loadBooks();
      }
      return;
    }
    if (k.wVirtualKeyCode == VK_BACK) {
      if (!editBuf_.empty()) {
        size_t p = editBuf_.size() - 1;
        while (p > 0 && ((unsigned char)editBuf_[p] & 0xC0) == 0x80) p--;
        editBuf_.erase(p);
      }
      dirty_ = true;
      return;
    }
    if (k.uChar.UnicodeChar >= 32) {
      wchar_t w[2] = {k.uChar.UnicodeChar, 0};
      editBuf_ += utf8ToWideLossless(w);
      dirty_ = true;
    }
    return;
  }

  const int page = std::max(1, contentHeight() - 2);
  auto moveSel = [&](int d) {
    int n = listCount();
    if (n <= 0) return;
    sel_ = std::max(0, std::min(n - 1, sel_ + d));
    clampSel();
    dirty_ = true;
  };
  auto scrollBy = [&](int d) {
    scroll_ = std::max(0, scroll_ + d);
    dirty_ = true;
  };
  bool selectable = (view_ == V_GRP && !inDetail_) ||
                    (view_ == V_BOOK && !inChapter_);

  switch (k.wVirtualKeyCode) {
    case VK_UP: if (selectable) moveSel(-1); else scrollBy(-1); return;
    case VK_DOWN: if (selectable) moveSel(1); else scrollBy(1); return;
    case VK_PRIOR: if (selectable) moveSel(-page); else scrollBy(-page); return;
    case VK_NEXT: if (selectable) moveSel(page); else scrollBy(page); return;
    case VK_HOME: sel_ = 0; scroll_ = 0; dirty_ = true; return;
    case VK_END: sel_ = 1 << 20; scroll_ = 1 << 20; clampSel(); dirty_ = true; return;
    case VK_TAB: {
      int step = (ctl & SHIFT_PRESSED) ? 3 : 1;
      view_ = (view_ + step) % 4;
      sel_ = 0;
      scroll_ = 0;
      inDetail_ = false;
      inChapter_ = false;
      inChapters_ = false;
      if (view_ == V_GRP && posts_.empty()) loadGrp();
      if (view_ == V_BOOK && books_.empty()) loadBooks();
      if (view_ == V_STATUS && !st_.ok) loadStatus();
      dirty_ = true;
      return;
    }
    case VK_ESCAPE: {
      if (view_ == V_GRP && inDetail_) {
        inDetail_ = false;
        scroll_ = sel_;
        dirty_ = true;
        return;
      }
      if (view_ == V_BOOK && inChapter_) {
        inChapter_ = false;
        inChapters_ = true;
        sel_ = chapIdx_;
        scroll_ = 0;
        clampSel();
        dirty_ = true;
        return;
      }
      if (view_ == V_BOOK && inChapters_) {
        inChapters_ = false;
        scroll_ = 0;
        dirty_ = true;
        return;
      }
      return;
    }
    case VK_RETURN: {
      if (view_ == V_GRP && !inDetail_ && sel_ < (int)posts_.size()) {
        loadDetail(posts_[(size_t)sel_].number);
        return;
      }
      if (view_ == V_BOOK && !inChapters_ && !inChapter_ && sel_ < (int)books_.size()) {
        loadChapters(sel_);
        return;
      }
      if (view_ == V_BOOK && inChapters_ && sel_ < (int)chapters_.size()) {
        chapIdx_ = sel_;
        inChapter_ = true;
        inChapters_ = false;
        scroll_ = 0;
        dirty_ = true;
        return;
      }
      return;
    }
    default: break;
  }

  wchar_t ch = (wchar_t)towlower((wint_t)k.uChar.UnicodeChar);
  if (ch == 0) return;
  switch (ch) {
    case 'q': PostQuitMessage(0); return;
    case 'j': if (selectable) moveSel(1); else scrollBy(1); return;
    case 'k': if (selectable) moveSel(-1); else scrollBy(-1); return;
    case 'g': sel_ = 0; scroll_ = 0; dirty_ = true; return;
    case 'G': sel_ = 1 << 20; scroll_ = 1 << 20; clampSel(); dirty_ = true; return;
    case '1': view_ = V_GRP; sel_ = 0; scroll_ = 0; inDetail_ = false; dirty_ = true; if (posts_.empty()) loadGrp(); return;
    case '2': view_ = V_BOOK; sel_ = 0; scroll_ = 0; inChapters_ = false; inChapter_ = false; dirty_ = true; if (books_.empty()) loadBooks(); return;
    case '3': view_ = V_STATUS; scroll_ = 0; dirty_ = true; if (!st_.ok) loadStatus(); return;
    case '4': view_ = V_HELP; scroll_ = 0; dirty_ = true; return;
    case '?': view_ = V_HELP; scroll_ = 0; dirty_ = true; return;
    case 'r': refreshCurrent(); dirty_ = true; return;
    case '/':
      if (view_ == V_GRP && !inDetail_) { edit_ = E_SEARCH_GRP; editBuf_ = query_; }
      else if (view_ == V_BOOK && !inChapters_ && !inChapter_) { edit_ = E_SEARCH_BOOK; editBuf_ = bookFilter_; }
      dirty_ = true;
      return;
    case 'c':
      if (view_ == V_GRP && inDetail_) {
        if (me_.empty()) { setMsg("写评论要先 bn login", A_WARN); dirty_ = true; return; }
        edit_ = E_COMMENT;
        editBuf_.clear();
        dirty_ = true;
        return;
      }
      if (view_ == V_GRP && !inDetail_) {
        static const char* order[] = {"", "公告", "新闻", "资讯", "技术", "活动", "随笔"};
        int cur = -1;
        for (int i = 0; i < 7; i++)
          if (catFilter_ == order[i]) cur = i;
        catFilter_ = order[(cur + 1) % 7];
        loadGrp();
        return;
      }
      return;
    case 's':
      if (view_ == V_GRP && !inDetail_) {
        if (sortKind_ == "default") sortKind_ = "created";
        else if (sortKind_ == "created") sortKind_ = "likes";
        else sortKind_ = "default";
        loadGrp();
      }
      return;
    case 'a':
      if (view_ == V_GRP && !inDetail_) { asc_ = !asc_; loadGrp(); }
      return;
    case 'u':
      if (view_ == V_GRP && inDetail_) {
        if (me_.empty()) { setMsg("点赞要先 bn login", A_WARN); dirty_ = true; return; }
        std::string id = detailId_;
        kick(
            [this, id]() {
              Ctx c = ctx();
              nlohmann::json out;
              std::string err;
              if (!grpUpvote(c, id, false, out, err)) { stage_.err = err; return; }
              stage_.msg = "已点赞";
              stage_.msgAttr = A_OK;
            },
            "点赞");
        return;
      }
      return;
    default: return;
  }
}

bool App::checkSize() {
  if (headless_) return false;
  CONSOLE_SCREEN_BUFFER_INFO ci;
  if (!GetConsoleScreenBufferInfo(hout_, &ci)) return false;
  int w = ci.srWindow.Right - ci.srWindow.Left + 1;
  int h = ci.srWindow.Bottom - ci.srWindow.Top + 1;
  bool offsets = aligned_ && (ci.srWindow.Left != 0 || ci.srWindow.Top != 0);
  if (w == lastW_ && h == lastH_ && !offsets) return false;
  lastW_ = w;
  lastH_ = h;
  // 窗口尺寸变了：VT 模式不用管缓冲区；老路径把缓冲区重新对齐成窗口大小
  if (!vtOk_) aligned_ = alignBuffer(hout_, COORD{(SHORT)w, (SHORT)h}) || aligned_;
  // 新窗口更窄时，旧尺寸写出的长行会被终端折行、留下残影；
  // 让下一帧整屏前带上清屏序列（和整帧同一次写出，不会闪白）
  sizeChanged_ = true;
  // 高度变了，可显示的行数也变了：把选中项拉回视野内，别让它留在屏幕外
  clampSel();
  int tw = 0, th = 0;
  if (queryTermSize(hin_, hout_, tw, th) && tw >= 20 && th >= 8) {
    termW_ = tw;
    termH_ = th;
  } else {
    termW_ = 0;
    termH_ = 0;
  }
  return true;
}

void App::loop() {
  for (;;) {
    poll();

    MSG m;
    bool quit = false;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) { quit = true; break; }
    }
    if (quit) return;

    if (busy_ || dirty_) {
      draw();
      dirty_ = false;
    }

    DWORD waited = WaitForSingleObject(hin_, 50);
    if (waited == WAIT_TIMEOUT) {
      if (checkSize()) dirty_ = true;
      continue;
    }
    if (waited == WAIT_OBJECT_0) {
      DWORD n = 0;
      if (GetNumberOfConsoleInputEvents(hin_, &n) && n > 0) {
        for (DWORD i = 0; i < n; i++) {
          INPUT_RECORD rec;
          DWORD got = 0;
          if (!ReadConsoleInputW(hin_, &rec, 1, &got) || got == 0) break;
          // 只认按键：鼠标 / 焦点 / 菜单事件一律丢弃
          if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
            handleKey(rec.Event.KeyEvent);
          else if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            checkSize();
            dirty_ = true;
          }
        }
      }
    }
  }
}

void App::headless(int w, int h) {
  headless_ = true;
  fakeW_ = w;
  fakeH_ = h;
  cfg_ = loadConfig();
  me_ = cfg_.login;
}

bool App::loadAllHeadless() {
  loadGrp();
  if (!posts_.empty()) loadDetail(posts_[0].number);
  loadBooks();
  if (!books_.empty()) loadChapters(0);
  loadStatus();
  return true;
}

std::vector<std::wstring> App::renderText() {
  std::vector<std::wstring> out;
  int saveSel = sel_;
  auto block = [&](const std::wstring& name, int v, bool det, bool chaps, bool chap) {
    view_ = v;
    inDetail_ = det;
    inChapters_ = chaps;
    inChapter_ = chap;
    scroll_ = 0;
    sel_ = listCount() > 1 ? 1 : 0;
    out.push_back(L"==== " + name + L"（宽 " + std::to_wstring(fakeW_) + L"）");
    out.push_back(L" bn " + WW(kVersion) + L"   [主站][书阁][状态][帮助]        @" + WW(me_));
    out.push_back(repeatW(L'─', fakeW_));
    std::vector<Row> rows = buildRows(fakeW_ - 2);
    int h = fakeH_ - 4;
    for (int i = 0; i < (int)rows.size() && i < h; i++) {
      std::wstring line = rows[(size_t)i].s;
      out.push_back((rows[(size_t)i].sel ? L"▸" : L" ") + line);
    }
    if ((int)rows.size() > h) out.push_back(L"  … 共 " + std::to_wstring(rows.size()) + L" 行");
    out.push_back(L"[" + WW(msg_) + L"]");
    out.push_back(L"");
  };
  block(L"主站列表", V_GRP, false, false, false);
  if (!detailBody_.empty()) block(L"文章详情", V_GRP, true, false, false);
  block(L"书阁列表", V_BOOK, false, false, false);
  if (!chapters_.empty()) {
    block(L"章节列表", V_BOOK, false, true, false);
    chapIdx_ = 0;
    block(L"章节正文", V_BOOK, false, false, true);
  }
  block(L"状态页", V_STATUS, false, false, false);
  block(L"帮助", V_HELP, false, false, false);
  sel_ = saveSel;
  return out;
}

int App::run() {
  if (!init()) return EX_FAIL;
  struct Guard {
    App* a;
    ~Guard() { a->shutdown(); }
  } guard{this};

  draw();
  loadGrp();
  loop();
  return EX_OK;
}

}  // namespace

bool stdoutIsTty() {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
}

int cmdTuiDump(const std::string& path, int w, int h) {
  App app;
  app.headless(w, h);
  app.loadAllHeadless();
  auto lines = app.renderText();
  std::string text;
  for (auto& l : lines) text += utf8ToWideLossless(l) + "\n";
  if (!writeFile(path, text)) return fail(EX_FAIL, "写不进 " + path);
  std::cout << "已写出：" << path << "（" << lines.size() << " 行）\n";
  return EX_OK;
}

int cmdTui() {
  if (!stdoutIsTty())
    return fail(EX_USAGE, "全屏界面需要在交互式终端里运行（bn tui）", "usage");
  App app;
  return app.run();
}

}  // namespace bn
