/**
 * @file   byte_ring_buffer.h
 * @brief  字节环形缓冲区实现
 *
 * 支持静态外部存储（栈/全局数组）或动态 PSRAM/SRAM 分配。
 * 线程不安全，适用于单线程事件循环场景。
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace wifi_uart {

/**
 * @brief 字节环形缓冲区
 *
 * 提供 push / peek / discard / clear 操作。存储介质可以是：
 * - PSRAM（可选，需 USE_PSRAM_BRIDGE_BUFFERS=1）
 * - 普通 malloc 分配的 SRAM
 * - 调用者提供的外部静态数组
 */
class ByteRingBuffer {
 public:
  ByteRingBuffer() = default;

  ~ByteRingBuffer() {
    release();
  }

  ByteRingBuffer(const ByteRingBuffer &) = delete;
  ByteRingBuffer &operator=(const ByteRingBuffer &) = delete;
  ByteRingBuffer(ByteRingBuffer &&) = delete;
  ByteRingBuffer &operator=(ByteRingBuffer &&) = delete;

  /**
   * @brief 动态分配缓冲区（可选 PSRAM）
   * @param capacity 缓冲区容量（字节）
   * @return true 初始化成功
   */
  [[nodiscard]]
  bool begin(size_t capacity);

  /**
   * @brief 使用调用者提供的外部缓冲区
   * @param storage 外部数组指针（不可为空）
   * @param capacity 外部数组大小
   * @return true 初始化成功
   */
  [[nodiscard]]
  bool begin(uint8_t *storage, size_t capacity);

  /** @brief 释放动态分配的存储（仅当 ownsStorage_==true 时） */
  void release();

  /** @brief 返回缓冲区总容量 */
  [[nodiscard]] size_t capacity() const {
    return capacity_;
  }

  /** @brief 返回是否使用 PSRAM 存储 */
  [[nodiscard]] bool usingPsram() const {
    return usingPsram_;
  }

  /** @brief 返回当前已使用字节数 */
  [[nodiscard]] size_t size() const {
    return count_;
  }

  /** @brief 返回剩余可用空间 */
  [[nodiscard]] size_t freeSpace() const {
    return capacity_ - count_;
  }

  /**
   * @brief 写入数据到缓冲区
   * @param data   数据源指针
   * @param length 期望写入字节数
   * @return 实际写入字节数（缓冲区满时会截断）
   */
  size_t push(const uint8_t *data, size_t length);

  /**
   * @brief 读取数据（不删除）
   * @param data   目标缓冲区
   * @param length 期望读取字节数
   * @return 实际读取字节数
   */
  size_t peek(uint8_t *data, size_t length) const;

  /**
   * @brief 丢弃缓冲区头部数据
   * @param length 丢弃字节数
   */
  void discard(size_t length);

  /** @brief 清空缓冲区内容 */
  void clear();

 private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
  bool ownsStorage_ = false;  ///< 是否拥有数据区所有权（需析构时释放）
  bool usingPsram_ = false;   ///< 是否使用 PSRAM
  size_t head_ = 0;           ///< 写入位置索引
  size_t tail_ = 0;           ///< 读取位置索引
  size_t count_ = 0;          ///< 当前数据字节数
};

}