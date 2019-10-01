type tsan_go.cpp ..\rtl\tsan_interface_atomic.cpp ..\rtl\tsan_clock.cpp ..\rtl\tsan_flags.cpp ..\rtl\tsan_md5.cpp ..\rtl\tsan_mutex.cpp ..\rtl\tsan_report.cpp ..\rtl\tsan_rtl.cpp ..\rtl\tsan_rtl_mutex.cpp ..\rtl\tsan_rtl_report.cpp ..\rtl\tsan_rtl_thread.cpp ..\rtl\tsan_rtl_proc.cpp ..\rtl\tsan_stat.cpp ..\rtl\tsan_suppressions.cpp ..\rtl\tsan_sync.cpp ..\rtl\tsan_stack_trace.cpp ..\..\sanitizer_common\sanitizer_allocator.cpp ..\..\sanitizer_common\sanitizer_common.cpp ..\..\sanitizer_common\sanitizer_flags.cpp ..\..\sanitizer_common\sanitizer_stacktrace.cpp ..\..\sanitizer_common\sanitizer_libc.cpp ..\..\sanitizer_common\sanitizer_printf.cpp ..\..\sanitizer_common\sanitizer_suppressions.cpp ..\..\sanitizer_common\sanitizer_thread_registry.cpp ..\rtl\tsan_platform_windows.cpp ..\..\sanitizer_common\sanitizer_win.cpp ..\..\sanitizer_common\sanitizer_deadlock_detector1.cpp ..\..\sanitizer_common\sanitizer_stackdepot.cpp ..\..\sanitizer_common\sanitizer_persistent_allocator.cpp ..\..\sanitizer_common\sanitizer_flag_parser.cpp ..\..\sanitizer_common\sanitizer_symbolizer.cpp ..\..\sanitizer_common\sanitizer_termination.cpp ..\..\sanitizer_common\sanitizer_file.cpp ..\..\sanitizer_common\sanitizer_symbolizer_report.cpp ..\rtl\tsan_external.cpp  > gotsan.cpp

gcc -c -o race_windows_amd64.syso gotsan.cpp -I..\rtl -I..\.. -I..\..\sanitizer_common -I..\..\..\include -m64 -Wall -fno-exceptions -fno-rtti -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 -DSANITIZER_GO=1 -DGetProcessMemoryInfo=K32GetProcessMemoryInfo -Wno-error=attributes -Wno-attributes -Wno-format -Wno-maybe-uninitialized -DSANITIZER_DEBUG=0 -O3 -fomit-frame-pointer -std=c++11
