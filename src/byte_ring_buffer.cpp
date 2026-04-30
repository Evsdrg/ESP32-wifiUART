/**
 * @file   byte_ring_buffer.cpp
 * @brief  环形缓冲区实现
 *
 * 线程不安全，适用于单线程事件循环。
 * 支持两种初始化模式：动态分配（可选 PSRAM）或外部提供存储。
 */

#include "byte_ring_buffer.h"
#include <Arduino.h>
#include <cstdlib>

namespace wifi_uart {

bool ByteRingBuffer::begin(size_t capacity) {
  release();

  if (capacity == 0) {
    return false;
  }

  // 优先尝试 PSRAM（大容量缓冲区场景），回退到普通 malloc
#if USE_PSRAM_BRIDGE_BUFFERS
  data_ = static_cast<uint8_t *>(ps_malloc(capacity));
  usingPsram_ = data_ != nullptr;
#endif
  if (data_ == nullptr) {
    data_ = static_cast<uint8_t *>(malloc(capacity));
    usingPsram_ = false;
  }

  if (data_ == nullptr) {
    capacity_ = 0;
    ownsStorage_ = false;
    usingPsram_ = false;
    return false;
  }

  capacity_ = capacity;
  ownsStorage_ = true;
  clear();
  return true;
}

bool ByteRingBuffer::begin(uint8_t *storage, size_t capacity) {
  release();

  if (storage == nullptr || capacity == 0) {
    return false;
  }

  data_ = storage;
  capacity_ = capacity;
  ownsStorage_ = false;   // 外部提供存储，析构时不释放
  usingPsram_ = false;
  clear();
  return true;
}

void ByteRingBuffer::release() {
  // 仅释放自我管理的动态分配内存
  if (ownsStorage_ && data_ != nullptr) {
    std::free(data_);
  }

  data_ = nullptr;
  capacity_ = 0;
  ownsStorage_ = false;
  usingPsram_ = false;
  head_ = 0;
  tail_ = 0;
  count_ = 0;
}

size_t ByteRingBuffer::push(const uint8_t *data, size_t length) {
  if (data_ == nullptr || capacity_ == 0) {
    return 0;
  }

  size_t written = 0;
  // 循环写入直到数据源耗尽或缓冲区满
  while (written < length && count_ < capacity_) {
    data_[head_] = data[written++];
    head_ = (head_ + 1) % capacity_;
    ++count_;
  }
  return written;
}

size_t ByteRingBuffer::peek(uint8_t *data, size_t length) const {
  if (data_ == nullptr || capacity_ == 0) {
    return 0;
  }

  // 从尾部（最早写入的位置）开始顺序读取，不移动读取指针
  const size_t available = min(length, count_);
  size_t index = tail_;
  for (size_t i = 0; i < available; ++i) {
    data[i] = data_[index];
    index = (index + 1) % capacity_;
  }
  return available;
}

void ByteRingBuffer::discard(size_t length) {
  if (capacity_ == 0) {
    return;
  }

  // 推进尾部指针，相当于丢弃已读数据
  const size_t discarded = min(length, count_);
  tail_ = (tail_ + discarded) % capacity_;
  count_ -= discarded;
}

void ByteRingBuffer::clear() {
  head_ = 0;
  tail_ = 0;
  count_ = 0;
  // 注意：不清除实际数据内容，只需逻辑指针归零
}

}