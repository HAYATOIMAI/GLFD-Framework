#include "InputSystem.h"

namespace GLFD::Core {
  InputSystem::InputSystem()
  {
    m_currentKeys.fill(0U);
  }
  void InputSystem::Update(HWND hwnd) {
    // 1. キーボード全状態を取得 (Win32 API)
    // GetKeyboardState は呼び出しスレッドのメッセージキューに基づく状態を取得する
    if (!GetKeyboardState(m_currentKeys.data())) {
      // 取得失敗時はクリアしておくなどの安全策
      m_currentKeys.fill(0);
    }

    // 2. 前に読んだ状態との差(押された瞬間・離された瞬間)を持ち越しの箱に足す (ECS 2-8)。
    //    刻みの頭の BeginStep で、その刻みの分として取り出される
    m_edges.Sample(m_currentKeys);

    // 3. マウス座標を取得
    POINT pt;

    if (GetCursorPos(&pt)) {
      // スクリーン座標からクライアント座標（ウィンドウ内座標）へ変換
      if (ScreenToClient(hwnd, &pt)) {
        m_mouseX = pt.x;
        m_mouseY = pt.y;
      }
    }
  }
  bool InputSystem::IsPressed(int key) const
  {
    return m_edges.IsPressed(key);
  }
  bool InputSystem::IsTriggered(int key) const
  {
    return m_edges.IsTriggered(key);
  }
  bool InputSystem::IsReleased(int key) const
  {
    return m_edges.IsReleased(key);
  }
}
