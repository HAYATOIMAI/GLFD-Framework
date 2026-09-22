#pragma once
// ---------------------------------------------------------------------------
//  JobSystem の試験用の差し込み点(ECS 2-4)
//
//  停止経路の競合を再現するハーネスが、**窓を広げる**(yield / 短い待ち)
//  ことと、**ログを出さずに観測する**(原子カウンタ)ことのために使う。
//
//  - `GLFD_JOBSYSTEM_PROBE` を定義したビルドでだけ `Probe::Hit` の呼び出しに
//    なる。**定義しなければ `((void)0)` に消え、引数の式も評価されない。**
//    本番(GameLib_conteinar.vcxproj)はこれを定義しない
//  - `Probe::Hit` の実体は**エンジン側に置かない**。定義するのはハーネスだけ。
//    本番に定義が紛れ込むとリンクが通ってしまうので、ここで宣言だけを持つ
// ---------------------------------------------------------------------------

#if defined(GLFD_JOBSYSTEM_PROBE)

#include <cstddef>

namespace GLFD::Thread::Probe {
  enum class Site : int {
    WorkerBeforeWait,   // ワーカーが眠ると決め、wait に入る直前(value = 待つ値)
    WorkerExit,         // ワーカーが主ループを抜けた
    StopBeforeNotify,   // Stop() が停止フラグを立て、ワーカーを起こす直前(value = 起こす前の値)
    StopAfterNotify,    // Stop() がワーカーを起こした直後
    Count
  };

  // ハーネスが定義する。value は差し込み点ごとの観測値(使わない点は 0)
  void Hit(Site site, std::size_t value) noexcept;
}

#define GLFD_JOB_PROBE(site, value) \
  ::GLFD::Thread::Probe::Hit(::GLFD::Thread::Probe::Site::site, (value))

#else

#define GLFD_JOB_PROBE(site, value) ((void)0)

#endif
