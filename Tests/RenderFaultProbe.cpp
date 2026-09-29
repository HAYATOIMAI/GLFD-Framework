/**
 * @file  RenderFaultProbe.cpp
 * @brief `RenderFaultProbe::Inject` の定義。**確認用のビルドにだけ入る** (ECS 2-9)
 *
 * @details
 *  vcxproj は `GLFDProbeDefines` に `GLFD_RENDER_FAULT_PROBE` が入っているときだけ、この .cpp を
 *  建てる。本番のビルドには入らないので、本番に `GLFD_RENDER_FAULT_PROBE` が紛れ込めば
 *  `Inject` の定義が無くリンクエラーになる。
 *
 *  ## 差し替えの指定: 環境変数 `GLFD_RENDER_FAULT`
 *  `,` で区切った規則の並び。規則は `点=値@何回目から[+何回]`。
 *   - 点: `present` / `test` / `map` / `reason`(`RenderFaultProbe::Point` の順)
 *   - 値: 16 進(`0x887A0005`)
 *   - 何回目: その点の呼び出しを 0 から数えた番号。`+何回` が無ければ、それ以降ずっと
 *  例: `present=0x887A0005@432,reason=0x887A0006@0`
 *      (433 回目の Present から消失を返し、GetDeviceRemovedReason は最初から HUNG を返す)
 *  指定が無い点は本物を呼ぶ。読めない指定は、起動時に stderr に書いて差し替えない。
 */

#include "../Source/Graphics/RenderFaultProbe.h"

#if !defined(GLFD_RENDER_FAULT_PROBE)
#error "RenderFaultProbe.cpp belongs only to the probe build (GLFD_RENDER_FAULT_PROBE)"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GLFD::Graphics::RenderFaultProbe {
  namespace {
    constexpr int kPoints = 4;
    constexpr int kMaxRules = 8;

    struct Rule {
      int           point = -1;
      HRESULT       value = S_OK;
      unsigned long from  = 0;
      unsigned long count = 0;   ///< 0 はずっと
    };

    struct State {
      bool          parsed = false;
      Rule          rules[kMaxRules]{};
      int           ruleCount = 0;
      unsigned long calls[kPoints]{};
    };

    State& GetState() noexcept {
      static State s;
      return s;
    }

    int PointOf(const char* name, std::size_t len) noexcept {
      static const char* const kNames[kPoints] = { "present", "test", "map", "reason" };
      for (int i = 0; i < kPoints; ++i) {
        if (std::strlen(kNames[i]) == len && std::strncmp(kNames[i], name, len) == 0) { return i; }
      }
      return -1;
    }

    void Parse(State& s) noexcept {
      s.parsed = true;
      char spec[512] = {};
      std::size_t len = 0;
      if (getenv_s(&len, spec, sizeof spec, "GLFD_RENDER_FAULT") != 0 || len == 0) { return; }
      const char* p = spec;
      while (*p != '\0' && s.ruleCount < kMaxRules) {
        const char* end = std::strchr(p, ',');
        const std::size_t n = (end != nullptr) ? static_cast<std::size_t>(end - p) : std::strlen(p);
        char one[128] = {};
        if (n >= sizeof one) { std::fprintf(stderr, "GLFD_RENDER_FAULT: rule too long\n"); return; }
        std::memcpy(one, p, n);
        const char* eq = std::strchr(one, '=');
        const char* at = std::strchr(one, '@');
        Rule r;
        if (eq != nullptr && at != nullptr && at > eq) {
          r.point = PointOf(one, static_cast<std::size_t>(eq - one));
          r.value = static_cast<HRESULT>(std::strtoul(eq + 1, nullptr, 16));
          char* after = nullptr;
          r.from = std::strtoul(at + 1, &after, 10);
          if (after != nullptr && *after == '+') { r.count = std::strtoul(after + 1, nullptr, 10); }
        }
        if (r.point < 0) { std::fprintf(stderr, "GLFD_RENDER_FAULT: cannot read rule '%s'\n", one); return; }
        s.rules[s.ruleCount++] = r;
        if (end == nullptr) { break; }
        p = end + 1;
      }
    }
  }

  bool Inject(Point point, HRESULT& replaced) noexcept {
    State& s = GetState();
    if (!s.parsed) { Parse(s); }
    const int index = static_cast<int>(point);
    if (index < 0 || index >= kPoints) { return false; }
    const unsigned long call = s.calls[index]++;
    for (int i = 0; i < s.ruleCount; ++i) {
      const Rule& r = s.rules[i];
      if (r.point != index || call < r.from) { continue; }
      if (r.count != 0 && call >= r.from + r.count) { continue; }
      replaced = r.value;
      return true;
    }
    return false;
  }
}
