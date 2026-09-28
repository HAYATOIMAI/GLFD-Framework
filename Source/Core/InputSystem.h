#pragma once
#include <array>
#include <cstdint>
#include <windows.h>

#include "KeyEdgeLatch.h"

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
    F5 = VK_F5,
    F6 = VK_F6,
    // 必要に応じて追加 (A-Zは 'A' でOK)
    MouseLeft = VK_LBUTTON,
    MouseRight = VK_RBUTTON
  };

  /**
   * @brief キーボードとマウスを読む
   * @details **フレームに 1 回 `Update` で読み、刻みの頭で `BeginStep` を呼ぶ** (ECS 2-8)。
   *          押された瞬間・離された瞬間は `KeyEdgeLatch` が持ち越し、ちょうど 1 回の刻みに届ける。
   *          2-7 までは `Update` が毎フレーム(= 毎回の `Update(dt)`)呼ばれ、前回との差で
   *          瞬間を決めていた。刻みが 0 回や 2 回のフレームがあると、その形では取りこぼすか
   *          2 回届く
   */
  class InputSystem {
  public:
    InputSystem();

    // フレームに 1 回呼ぶ（状態を読む）
    void Update(HWND hwnd);

    // 刻みの頭で呼ぶ。押された瞬間・離された瞬間をこの刻みの分として取り出す (ECS 2-8)
    void BeginStep() noexcept { m_edges.BeginStep(); }

    // キーが押されているか (Hold)。最後に読んだ状態
    bool IsPressed(int key) const;

    // キーが押された瞬間か (Trigger)。この刻みに届いた分
    bool IsTriggered(int key) const;

    // キーが離された瞬間か (Release)。この刻みに届いた分
    bool IsReleased(int key) const;

    // マウス座標 (クライアント領域基準)
    int GetMouseX() const { return m_mouseX; }
    int GetMouseY() const { return m_mouseY; }

  private:
    // キーボード状態 (256キー)
    // bit 7 (0x80) が押下フラグ
    KeyEdgeLatch::KeyStates m_currentKeys;
    // 押された瞬間・離された瞬間の持ち越し (ECS 2-8)
    KeyEdgeLatch m_edges;

    int m_mouseX = 0;
    int m_mouseY = 0;
  };
}
