#pragma once
#include <utility>

namespace GLFD::Template {
  template <typename T>
  class Singleton {
  public:
    /**
     * @brief シングルトンインスタンスへの参照を取得
     * @return T& インスタンスへの参照
     */
    static T& GetInstance() noexcept(std::is_nothrow_constructible_v<T>) {
      // Magic Static: この静的変数の初期化はスレッドセーフかつ一度しか行われない
      static T instance;
      return instance;
    }
  protected:
    /**
     * @brief デフォルトコンストラクタ
     * @details  外部からの直接的なインスタンス化を防止
     * 
     */
    Singleton() = default;

    /**
     * @brief デフォルトデストラクタ
     */
    virtual ~Singleton() = default;

  public:
    // インスタンスの唯一性を保証するため、コピーとムーブ操作を明確に禁止

    /**
     * @brief コピーコンストラクタ（禁止）
     */
    Singleton(const Singleton&) = delete;

    /**
     * @brief コピー代入演算子（禁止）
     */
    Singleton& operator=(const Singleton&) = delete;

    /**
     * @brief ムーブコンストラクタ（禁止）
     */
    Singleton(Singleton&&) = delete;

    /**
     * @brief ムーブ代入演算子（禁止）
     */
    Singleton& operator=(Singleton&&) = delete;
  };
}