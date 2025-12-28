#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX // std::min/max との衝突回避
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring> // memset

namespace GLFD::Graphics {
  class SimpleWindow {
  public:
    SimpleWindow(const std::wstring& title, int width, int height);
    ~SimpleWindow();

    // 画面を特定の色でクリア
    void Clear(uint32_t color);

    // ピクセル描画 (範囲チェックなし・最速版)
    void DrawPixel(int x, int y, uint32_t color);

    // バッファの内容をウィンドウに転送
    void Present();

    // メッセージループを処理
    bool ProcessMessages();

    HWND GetHWND() const { return m_hwnd; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

  private:
    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;

    // 描画バッファ
    void* m_bufferMemory = nullptr;
    BITMAPINFO m_bmi = {};

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void InitBuffer();
  };
}
