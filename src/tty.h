// tty.h — 全屏终端界面（纯键盘，不使用鼠标）
#pragma once
#include <string>

namespace bn {

bool stdoutIsTty();

/* 进入全屏交互界面；非交互终端返回用法错误 */
int cmdTui();

/* 无窗口排版自检：把各页渲染成纯文本写盘（不碰控制台） */
int cmdTuiDump(const std::string& path, int width, int height);

}  // namespace bn
