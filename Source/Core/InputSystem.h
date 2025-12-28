#pragma once
#include <array>
#include <cstdint>
#include <windows.h>

namespace GLFD::Core {
  // キーコードのエイリアス（Win32準拠）
  enum KeyCode : int {
    Space = VK_SPACE,
    Enter = VK_RETURN,
    Escape = VK_ESCAPE,
    Left = VK_LEFT,
    Up = VK_UP,
    Right = VK_RIGHT,
    Down = VK_DOWN,
    // 必要に応じて追加 (A-Zは 'A' でOK)
    MouseLeft = VK_LBUTTON,
    MouseRight = VK_RBUTTON
  };

  class InputSystem {
  public:
    InputSystem();

    // 毎フレームの最初に呼ぶ（状態更新）
    void Update(HWND hwnd);

    // キーが押されているか (Hold)
    bool IsPressed(int key) const;

    // キーが押された瞬間か (Trigger)
    bool IsTriggered(int key) const;

    // キーが離された瞬間か (Release)
    bool IsReleased(int key) const;

    // マウス座標 (クライアント領域基準)
    int GetMouseX() const { return m_mouseX; }
    int GetMouseY() const { return m_mouseY; }

  private:
    // キーボード状態 (256キー)
    // bit 7 (0x80) が押下フラグ
    std::array<uint8_t, 256> m_currentKeys;
    std::array<uint8_t, 256> m_prevKeys;

    int m_mouseX = 0;
    int m_mouseY = 0;
  };
}
