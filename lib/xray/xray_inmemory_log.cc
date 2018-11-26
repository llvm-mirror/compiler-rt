//===-- xray_inmemory_log.cc ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of XRay, a dynamic runtime instrumentation system.
//
// Implementation of a simple in-memory log of XRay events. This defines a
// logging function that's compatible with the XRay handler interface, and
// routines for exporting data to files.
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#if defined(__x86_64__)
#include "xray_x86_64.h"
#elif defined(__arm__) || defined(__aarch64__)
#include "xray_emulate_tsc.h"
#else
#error "Unsupported CPU Architecture"
#endif /* Architecture-specific inline intrinsics */

#include "sanitizer_common/sanitizer_libc.h"
#include "xray/xray_records.h"
#include "xray_defs.h"
#include "xray_flags.h"
#include "xray_interface_internal.h"

// __xray_InMemoryRawLog will use a thread-local aligned buffer capped to a
// certain size (32kb by default) and use it as if it were a circular buffer for
// events. We store simple fixed-sized entries in the log for external analysis.

extern "C" {
void __xray_InMemoryRawLog(int32_t FuncId,
                           XRayEntryType Type) XRAY_NEVER_INSTRUMENT;
}

namespace __xray {

std::mutex LogMutex;

static void retryingWriteAll(int Fd, char *Begin,
                             char *End) XRAY_NEVER_INSTRUMENT {
  if (Begin == End)
    return;
  auto TotalBytes = std::distance(Begin, End);
  while (auto Written = write(Fd, Begin, TotalBytes)) {
    if (Written < 0) {
      if (errno == EINTR)
        continue; // Try again.
      Report("Failed to write; errno = %d\n", errno);
      return;
    }
    TotalBytes -= Written;
    if (TotalBytes == 0)
      break;
    Begin += Written;
  }
}

class ThreadExitFlusher {
  int Fd;
  XRayRecord *Start;
  size_t &Offset;

public:
  explicit ThreadExitFlusher(int Fd, XRayRecord *Start,
                             size_t &Offset) XRAY_NEVER_INSTRUMENT
      : Fd(Fd),
        Start(Start),
        Offset(Offset) {}

  ~ThreadExitFlusher() XRAY_NEVER_INSTRUMENT {
    std::lock_guard<std::mutex> L(LogMutex);
    if (Fd > 0 && Start != nullptr) {
      retryingWriteAll(Fd, reinterpret_cast<char *>(Start),
                       reinterpret_cast<char *>(Start + Offset));
      // Because this thread's exit could be the last one trying to write to the
      // file and that we're not able to close out the file properly, we sync
      // instead and hope that the pending writes are flushed as the thread
      // exits.
      fsync(Fd);
    }
  }
};

} // namespace __xray

using namespace __xray;

void PrintToStdErr(const char *Buffer) XRAY_NEVER_INSTRUMENT {
  fprintf(stderr, "%s", Buffer);
}

static int __xray_OpenLogFile() XRAY_NEVER_INSTRUMENT {
  // FIXME: Figure out how to make this less stderr-dependent.
  SetPrintfAndReportCallback(PrintToStdErr);
  // Open a temporary file once for the log.
  static char TmpFilename[256] = {};
  static char TmpWildcardPattern[] = "XXXXXX";
  auto Argv = GetArgv();
  const char *Progname = Argv[0] == nullptr ? "(unknown)" : Argv[0];
  const char *LastSlash = internal_strrchr(Progname, '/');

  if (LastSlash != nullptr)
    Progname = LastSlash + 1;

  const int HalfLength = sizeof(TmpFilename) / 2 - sizeof(TmpWildcardPattern);
  int NeededLength = internal_snprintf(TmpFilename, sizeof(TmpFilename),
                                       "%.*s%.*s.%s",
                                       HalfLength, flags()->xray_logfile_base,
                                       HalfLength, Progname,
                                       TmpWildcardPattern);
  if (NeededLength > int(sizeof(TmpFilename))) {
    Report("XRay log file name too long (%d): %s\n", NeededLength, TmpFilename);
    return -1;
  }
  int Fd = mkstemp(TmpFilename);
  if (Fd == -1) {
    Report("XRay: Failed opening temporary file '%s'; not logging events.\n",
           TmpFilename);
    return -1;
  }
  if (Verbosity())
    fprintf(stderr, "XRay: Log file in '%s'\n", TmpFilename);

  // Since we're here, we get to write the header. We set it up so that the
  // header will only be written once, at the start, and let the threads
  // logging do writes which just append.
  XRayFileHeader Header;
  Header.Version = 1;
  Header.Type = FileTypes::NAIVE_LOG;
  Header.CycleFrequency = __xray::cycleFrequency();

  // FIXME: Actually check whether we have 'constant_tsc' and 'nonstop_tsc'
  // before setting the values in the header.
  Header.ConstantTSC = 1;
  Header.NonstopTSC = 1;
  retryingWriteAll(Fd, reinterpret_cast<char *>(&Header),
                   reinterpret_cast<char *>(&Header) + sizeof(Header));
  return Fd;
}

void __xray_InMemoryRawLog(int32_t FuncId,
                           XRayEntryType Type) XRAY_NEVER_INSTRUMENT {
  using Buffer =
      std::aligned_storage<sizeof(XRayRecord), alignof(XRayRecord)>::type;
  static constexpr size_t BuffLen = 1024;
  thread_local static Buffer InMemoryBuffer[BuffLen] = {};
  thread_local static size_t Offset = 0;
  static int Fd = __xray_OpenLogFile();
  if (Fd == -1)
    return;
  thread_local __xray::ThreadExitFlusher Flusher(
      Fd, reinterpret_cast<__xray::XRayRecord *>(InMemoryBuffer), Offset);
  thread_local pid_t TId = syscall(SYS_gettid);

  // First we get the useful data, and stuff it into the already aligned buffer
  // through a pointer offset.
  auto &R = reinterpret_cast<__xray::XRayRecord *>(InMemoryBuffer)[Offset];
  R.RecordType = RecordTypes::NORMAL;
  R.TSC = __xray::readTSC(R.CPU);
  R.TId = TId;
  R.Type = Type;
  R.FuncId = FuncId;
  ++Offset;
  if (Offset == BuffLen) {
    std::lock_guard<std::mutex> L(LogMutex);
    auto RecordBuffer = reinterpret_cast<__xray::XRayRecord *>(InMemoryBuffer);
    retryingWriteAll(Fd, reinterpret_cast<char *>(RecordBuffer),
                     reinterpret_cast<char *>(RecordBuffer + Offset));
    Offset = 0;
  }
}

static auto Unused = [] {
  if (flags()->xray_naive_log)
    __xray_set_handler(__xray_InMemoryRawLog);
  return true;
}();
