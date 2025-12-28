#include "SimpleWindow.h"

namespace GLFD::Graphics {
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
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    // 3. ウィンドウ作成
    m_hwnd = CreateWindowEx(
      0, L"GameLibWindow", title.c_str(),
      WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      CW_USEDEFAULT, CW_USEDEFAULT,
      rect.right - rect.left, rect.bottom - rect.top,
      nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    // 4. 描画バッファ (DIBSection) の初期化
    InitBuffer();
  }
  SimpleWindow::~SimpleWindow() {
    if (m_bufferMemory) VirtualFree(m_bufferMemory, 0, MEM_RELEASE);
  }

  void SimpleWindow::Clear(uint32_t color) {
    // 32bitカラー (0x00RRGGBB)
    // 高速化のため、本来はSIMDやmemset32を使うべきだが、今回はループで
    // または黒(0)ならmemsetで一撃
    if (color == 0) {
      std::memset(m_bufferMemory, 0, m_width * m_height * sizeof(uint32_t));
    }
    else {
     auto* pixel = static_cast<uint32_t*>(m_bufferMemory);
      for (int i = 0; i < m_width * m_height; ++i) {
        *pixel++ = color;
      }
    }
  }
  void SimpleWindow::DrawPixel(int x, int y, uint32_t color)
  {
    if (x >= 0 && x < m_width && y >= 0 && y < m_height) {
      // メモリは左下が原点ではなく、左上が原点になるようにDIBを作成するが
      // ここでは高さ方向を反転させて調整。
      // *m_bufferMemory は (0,0) = 左上 とする。
      auto* pixel = static_cast<uint32_t*>(m_bufferMemory);
      pixel[y * m_width + x] = color;
    }
  }
  void SimpleWindow::Present() {
    HDC hdc = GetDC(m_hwnd);

    StretchDIBits(
      hdc,
      0, 0, m_width, m_height,
      0, 0, m_width, m_height,
      m_bufferMemory, &m_bmi,
      DIB_RGB_COLORS, SRCCOPY
    );
    ReleaseDC(m_hwnd, hdc);
  }

  bool SimpleWindow::ProcessMessages() {
    MSG msg = {};

    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return false;
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    return true;
  }

  LRESULT SimpleWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }

  void SimpleWindow::InitBuffer() {
    m_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    m_bmi.bmiHeader.biWidth = m_width;
    m_bmi.bmiHeader.biHeight = -m_height;
    m_bmi.bmiHeader.biPlanes = 1;
    m_bmi.bmiHeader.biBitCount = 32; // 32bit color (XRGB)
    m_bmi.bmiHeader.biCompression = BI_RGB;

    // メモリ確保 (VirtualAllocでページ境界に確保すると効率が良い)
    size_t size = m_width * m_height * sizeof(uint32_t);
    m_bufferMemory = VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);
  }
}