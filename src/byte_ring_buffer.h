#pragma once

#include <cstdint>
#include <cstddef>

namespace wifi_uart {

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

  [[nodiscard]]
  bool begin(size_t capacity);

  [[nodiscard]]
  bool begin(uint8_t *storage, size_t capacity);

  void release();

  [[nodiscard]] size_t capacity() const {
    return capacity_;
  }

  [[nodiscard]] bool usingPsram() const {
    return usingPsram_;
  }

  [[nodiscard]] size_t size() const {
    return count_;
  }

  [[nodiscard]] size_t freeSpace() const {
    return capacity_ - count_;
  }

  size_t push(const uint8_t *data, size_t length);

  size_t peek(uint8_t *data, size_t length) const;

  void discard(size_t length);

  void clear();

 private:
  uint8_t *data_ = nullptr;
  size_t capacity_ = 0;
  bool ownsStorage_ = false;
  bool usingPsram_ = false;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
};

}