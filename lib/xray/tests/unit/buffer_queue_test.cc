//===-- buffer_queue_test.cc ----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of XRay, a function call tracing system.
//
//===----------------------------------------------------------------------===//
#include "xray_buffer_queue.h"
#include "gtest/gtest.h"

#include <future>
#include <system_error>
#include <unistd.h>

namespace __xray {

static constexpr size_t kSize = 4096;

TEST(BufferQueueTest, API) { BufferQueue Buffers(kSize, 1); }

TEST(BufferQueueTest, GetAndRelease) {
  BufferQueue Buffers(kSize, 1);
  BufferQueue::Buffer Buf;
  ASSERT_EQ(Buffers.getBuffer(Buf), std::error_code());
  ASSERT_NE(nullptr, Buf.Buffer);
  ASSERT_EQ(Buffers.releaseBuffer(Buf), std::error_code());
  ASSERT_EQ(nullptr, Buf.Buffer);
}

TEST(BufferQueueTest, GetUntilFailed) {
  BufferQueue Buffers(kSize, 1);
  BufferQueue::Buffer Buf0;
  EXPECT_EQ(Buffers.getBuffer(Buf0), std::error_code());
  BufferQueue::Buffer Buf1;
  EXPECT_EQ(std::errc::not_enough_memory, Buffers.getBuffer(Buf1));
  EXPECT_EQ(Buffers.releaseBuffer(Buf0), std::error_code());
}

TEST(BufferQueueTest, ReleaseUnknown) {
  BufferQueue Buffers(kSize, 1);
  BufferQueue::Buffer Buf;
  Buf.Buffer = reinterpret_cast<void *>(0xdeadbeef);
  Buf.Size = kSize;
  EXPECT_EQ(std::errc::argument_out_of_domain, Buffers.releaseBuffer(Buf));
}

TEST(BufferQueueTest, ErrorsWhenFinalising) {
  BufferQueue Buffers(kSize, 2);
  BufferQueue::Buffer Buf;
  ASSERT_EQ(Buffers.getBuffer(Buf), std::error_code());
  ASSERT_NE(nullptr, Buf.Buffer);
  ASSERT_EQ(Buffers.finalize(), std::error_code());
  BufferQueue::Buffer OtherBuf;
  ASSERT_EQ(std::errc::state_not_recoverable, Buffers.getBuffer(OtherBuf));
  ASSERT_EQ(std::errc::state_not_recoverable, Buffers.finalize());
  ASSERT_EQ(Buffers.releaseBuffer(Buf), std::error_code());
}

TEST(BufferQueueTest, MultiThreaded) {
  BufferQueue Buffers(kSize, 100);
  auto F = [&] {
    BufferQueue::Buffer B;
    while (!Buffers.getBuffer(B)) {
      Buffers.releaseBuffer(B);
    }
  };
  auto T0 = std::async(std::launch::async, F);
  auto T1 = std::async(std::launch::async, F);
  auto T2 = std::async(std::launch::async, [&] {
    while (!Buffers.finalize())
      ;
  });
  F();
}

} // namespace __xray
