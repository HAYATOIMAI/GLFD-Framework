#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX // std::min/max との衝突回避
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace GLFD::Graphics {
  class SimpleWindow {
  public:
    SimpleWindow(const std::wstring& title, int width, int height);
    ~SimpleWindow();

    // メッセージループを処理
    bool ProcessMessages();

    HWND GetHWND() const { return m_hwnd; }
    // 窓があるモニターのリフレッシュレート (Hz)。分からなければ 0 (ECS 2-8)
    int MonitorRefreshHz() const;
    // 最小化されているか (ECS 2-8)
    bool IsMinimized() const { return m_hwnd != nullptr && IsIconic(m_hwnd) != FALSE; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

  private:
    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  };
}
