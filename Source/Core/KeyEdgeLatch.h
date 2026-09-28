#pragma once

/**
 * @file  KeyEdgeLatch.h
 * @brief 押された瞬間・離された瞬間を、**ちょうど 1 回の刻みに**届ける (ECS 2-8)
 *
 * @details
 *  入力は**1 フレームに 1 回**読む(`Sample`)。シミュレーションは固定の刻みで進むので、
 *  1 フレームに刻みが 0 回のことも複数回のこともある (`SimulationClock.h`)。
 *  2-7 までのように「前の `Update` との差」で押された瞬間を決めると、
 *
 *  - 刻みが 0 回のフレームで押された瞬間は、**誰にも見られずに消える**
 *  - 刻みが 2 回のフレームでは、**同じ押下を 2 回処理する**(シーンを 2 回切り替える)
 *
 *  そこで、読んだときに見つけた瞬間を**持ち越しの箱**に貯め、刻みの頭(`BeginStep`)で
 *  その刻みの分として取り出して箱を空にする。
 *
 *  - 刻みが 0 回のフレームの瞬間は箱に残り、**次のフレームの最初の刻み**に届く
 *  - 同じフレームの 2 回目以降の刻みは、箱が空なので何も見ない
 *
 *  押されている間(`IsPressed`)は瞬間ではなく状態なので、最後に読んだ値をそのまま返す。
 *
 *  **Win32 を引き込まない。** キーの状態は呼ぶ側(`InputSystem` が `GetKeyboardState` で)
 *  読んで渡す。テストは状態の列を差し込んで確かめる (T-ECS-41)。
 */

#include <array>
#include <cstddef>
#include <cstdint>

namespace GLFD::Core {

  class KeyEdgeLatch {
  public:
    static constexpr int kKeyCount = 256;

    /// `GetKeyboardState` と同じ形: 各キー 1 バイト、最上位ビット (0x80) が押されている
    using KeyStates = std::array<std::uint8_t, kKeyCount>;

    /// 1 フレームに 1 回、読んだ状態を渡す。前に読んだ状態との差を持ち越しの箱に足す
    void Sample(const KeyStates& now) noexcept {
      for (int key = 0; key < kKeyCount; ++key) {
        const bool down    = (now[static_cast<std::size_t>(key)] & 0x80u) != 0u;
        const bool wasDown = Test(m_held, key);
        if (down && !wasDown) { Set(m_pendingPressed, key); }
        if (!down && wasDown) { Set(m_pendingReleased, key); }
        Assign(m_held, key, down);
      }
    }

    /// 刻みの頭で呼ぶ。持ち越しの箱をこの刻みの分として取り出し、箱を空にする
    void BeginStep() noexcept {
      m_stepPressed     = m_pendingPressed;
      m_stepReleased    = m_pendingReleased;
      m_pendingPressed  = Bits{};
      m_pendingReleased = Bits{};
    }

    /// この刻みで押された瞬間か
    [[nodiscard]] bool IsTriggered(int key) const noexcept { return InRange(key) && Test(m_stepPressed, key); }
    /// この刻みで離された瞬間か
    [[nodiscard]] bool IsReleased(int key) const noexcept { return InRange(key) && Test(m_stepReleased, key); }
    /// 最後に読んだとき押されていたか(状態)
    [[nodiscard]] bool IsPressed(int key) const noexcept { return InRange(key) && Test(m_held, key); }

  private:
    using Bits = std::array<std::uint64_t, kKeyCount / 64>;

    static bool InRange(int key) noexcept { return key >= 0 && key < kKeyCount; }
    static bool Test(const Bits& b, int key) noexcept {
      return ((b[static_cast<std::size_t>(key) / 64u] >> (static_cast<unsigned>(key) % 64u)) & 1u) != 0u;
    }
    static void Set(Bits& b, int key) noexcept {
      b[static_cast<std::size_t>(key) / 64u] |= (std::uint64_t{ 1 } << (static_cast<unsigned>(key) % 64u));
    }
    static void Assign(Bits& b, int key, bool on) noexcept {
      const std::uint64_t mask = std::uint64_t{ 1 } << (static_cast<unsigned>(key) % 64u);
      std::uint64_t& word = b[static_cast<std::size_t>(key) / 64u];
      word = on ? (word | mask) : (word & ~mask);
    }

    Bits m_held{};
    Bits m_pendingPressed{};
    Bits m_pendingReleased{};
    Bits m_stepPressed{};
    Bits m_stepReleased{};
  };

}
