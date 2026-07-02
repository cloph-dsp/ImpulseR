#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <algorithm>

namespace impulser {

/**
 * Lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
 * 
 * Thread-safe for one producer thread and one consumer thread without locks.
 * Uses atomic operations for head/tail pointers with appropriate memory ordering.
 * 
 * @tparam T Element type (must be trivially copyable)
 */
template<typename T>
class RingBuffer {
public:
    /**
     * Construct a ring buffer with the specified capacity.
     * Capacity must be a power of 2 for efficient modulo operation.
     * 
     * @param capacity Number of elements (must be power of 2)
     */
    explicit RingBuffer(size_t capacity)
        : mCapacity(capacity)
        , mMask(capacity - 1)
        , mBuffer(new T[capacity])
        , mHead(0)
        , mTail(0)
    {
        // Verify capacity is power of 2
        if ((capacity & mMask) != 0) {
            delete[] mBuffer;
            throw std::invalid_argument("RingBuffer capacity must be power of 2");
        }
    }

    ~RingBuffer() {
        delete[] mBuffer;
    }

    // Non-copyable
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /**
     * Write elements to the buffer (producer thread only).
     * 
     * @param data Pointer to data to write
     * @param count Number of elements to write
     * @return Number of elements actually written (may be less than count if buffer is full)
     */
    size_t write(const T* data, size_t count) {
        const size_t head = mHead.load(std::memory_order_relaxed);
        const size_t tail = mTail.load(std::memory_order_acquire);
        
        const size_t available = mCapacity - (head - tail);
        const size_t toWrite = std::min(count, available);
        
        if (toWrite == 0) {
            return 0;
        }
        
        // Write data in two parts if it wraps around
        const size_t firstPart = std::min(toWrite, mCapacity - (head & mMask));
        const size_t secondPart = toWrite - firstPart;
        
        std::memcpy(mBuffer + (head & mMask), data, firstPart * sizeof(T));
        if (secondPart > 0) {
            std::memcpy(mBuffer, data + firstPart, secondPart * sizeof(T));
        }
        
        // Update head with release semantics
        mHead.store(head + toWrite, std::memory_order_release);
        
        return toWrite;
    }

    /**
     * Read elements from the buffer (consumer thread only).
     * 
     * @param data Pointer to buffer to read into
     * @param count Number of elements to read
     * @return Number of elements actually read (may be less than count if buffer is empty)
     */
    size_t read(T* data, size_t count) {
        const size_t tail = mTail.load(std::memory_order_relaxed);
        const size_t head = mHead.load(std::memory_order_acquire);
        
        const size_t available = head - tail;
        const size_t toRead = std::min(count, available);
        
        if (toRead == 0) {
            return 0;
        }
        
        // Read data in two parts if it wraps around
        const size_t firstPart = std::min(toRead, mCapacity - (tail & mMask));
        const size_t secondPart = toRead - firstPart;
        
        std::memcpy(data, mBuffer + (tail & mMask), firstPart * sizeof(T));
        if (secondPart > 0) {
            std::memcpy(data + firstPart, mBuffer, secondPart * sizeof(T));
        }
        
        // Update tail with release semantics
        mTail.store(tail + toRead, std::memory_order_release);
        
        return toRead;
    }

    /**
     * Peek at elements without removing them (consumer thread only).
     * 
     * @param data Pointer to buffer to read into
     * @param count Number of elements to peek
     * @return Number of elements actually available to peek
     */
    size_t peek(T* data, size_t count) const {
        const size_t tail = mTail.load(std::memory_order_relaxed);
        const size_t head = mHead.load(std::memory_order_acquire);
        
        const size_t available = head - tail;
        const size_t toPeek = std::min(count, available);
        
        if (toPeek == 0) {
            return 0;
        }
        
        // Peek data in two parts if it wraps around
        const size_t firstPart = std::min(toPeek, mCapacity - (tail & mMask));
        const size_t secondPart = toPeek - firstPart;
        
        std::memcpy(data, mBuffer + (tail & mMask), firstPart * sizeof(T));
        if (secondPart > 0) {
            std::memcpy(data + firstPart, mBuffer, secondPart * sizeof(T));
        }
        
        return toPeek;
    }

    /**
     * Get the number of elements available for reading.
     */
    size_t available() const {
        const size_t head = mHead.load(std::memory_order_acquire);
        const size_t tail = mTail.load(std::memory_order_acquire);
        return head - tail;
    }

    /**
     * Get the number of elements that can be written.
     */
    size_t free() const {
        const size_t head = mHead.load(std::memory_order_acquire);
        const size_t tail = mTail.load(std::memory_order_acquire);
        return mCapacity - (head - tail);
    }

    /**
     * Get the total capacity of the buffer.
     */
    size_t capacity() const {
        return mCapacity;
    }

    /**
     * Clear the buffer (consumer thread only).
     */
    void clear() {
        mTail.store(mHead.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    const size_t mCapacity;
    const size_t mMask;
    
    // Buffer aligned to cache line for optimal memory access
    alignas(64) T* mBuffer;
    
    // Align head/tail to avoid false sharing
    alignas(64) std::atomic<size_t> mHead;
    alignas(64) std::atomic<size_t> mTail;
};

} // namespace impulser
