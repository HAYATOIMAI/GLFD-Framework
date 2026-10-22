#include "SimpleWindow.h"

namespace GLFD::Graphics {
  namespace {
    /// **枠の大きさを利用者が変えられない窓** (ECS 2-13 の判断 D3)。`WS_OVERLAPPEDWINDOW` から
    /// 枠のドラッグ(`WS_THICKFRAME`)と最大化(`WS_MAXIMIZEBOX`)を外す。最小化は残す
    /// (2-8 の「隠れている間は止める」が使う)。
    /// 2-12 までは枠を変えられたが、スワップチェーン・ビューポート・`GetWidth/GetHeight` は起動時の
    /// 大きさのままで、絵は引き伸ばされ、クリックもずれた(2-13 手順1 で実測)。
    /// 本当に追随させる(`WM_SIZE` → `ResizeBuffers`)のは別のフェーズ
    constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
  }

  SimpleWindow::SimpleWindow(const std::wstring& title, int width, int height) : 
    m_width(width), m_height(height) {
    // 1. ウィンドウクラスの登録
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"GameLibWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);

    // 2. ウィンドウサイズの調整（クライアント領域を指定サイズにする）
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, kWindowStyle, FALSE);

    // 3. ウィンドウ作成
    m_hwnd = CreateWindowEx(
      0, L"GameLibWindow", title.c_str(),
      kWindowStyle | WS_VISIBLE,
      CW_USEDEFAULT, CW_USEDEFAULT,
      rect.right - rect.left, rect.bottom - rect.top,
      nullptr, nullptr, GetModuleHandle(nullptr), this
    );
  }

  SimpleWindow::~SimpleWindow() = default;

  bool SimpleWindow::ProcessMessages() {
    MSG msg = {};

    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return false;
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    return true;
  }

  int SimpleWindow::MonitorRefreshHz() const {
    // 起動時に 1 回、観測の行に載せるためだけに読む (ECS 2-8)。窓モードの垂直同期は
    // このモニターの値で待つ(スワップチェーンの RefreshRate = 60 は効いていない。2-8 手順1)
    HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) { return 0; }
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) { return 0; }
    // 0 と 1 は「ハードウェアの既定」の意味で、実際の値ではない
    return (mode.dmDisplayFrequency > 1) ? static_cast<int>(mode.dmDisplayFrequency) : 0;
  }

  LRESULT SimpleWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}