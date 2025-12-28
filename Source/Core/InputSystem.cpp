#include "InputSystem.h"

namespace GLFD::Core {
  InputSystem::InputSystem()
  {
    m_currentKeys.fill(0U);
    m_prevKeys.fill(0U);
  }
  void InputSystem::Update(HWND hwnd) {
    // 1. 現在の状態を前回の状態として保存
    m_prevKeys = m_currentKeys;

    // 2. キーボード全状態を取得 (Win32 API)
    // GetKeyboardState は呼び出しスレッドのメッセージキューに基づく状態を取得する
    if (!GetKeyboardState(m_currentKeys.data())) {
      // 取得失敗時はクリアしておくなどの安全策
      m_currentKeys.fill(0);
    }

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
    if (key < 0 || key >= 256) return false;
    // 最上位ビットが1なら押されている
    return (m_currentKeys[key] & 0x80) != 0;
  }
  bool InputSystem::IsTriggered(int key) const
  {
    if (key < 0 || key >= 256) return false;
    // 「今は押されている」かつ「前は押されていなかった」
    return ((m_currentKeys[key] & 0x80) != 0) && ((m_prevKeys[key] & 0x80) == 0);
  }
  bool InputSystem::IsReleased(int key) const
  {
    if (key < 0 || key >= 256) return false;
    // 「今は押されていない」かつ「前は押されていた」
    return ((m_currentKeys[key] & 0x80) == 0) && ((m_prevKeys[key] & 0x80) != 0);
  }
}
