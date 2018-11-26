// RUN: %clang_tsan %s -o %t -framework Foundation
// RUN: %env_tsan_opts=ignore_interceptors_accesses=1 %run %t 2>&1 | FileCheck %s

#import <Foundation/Foundation.h>

long global;

int main(int argc, const char *argv[]) {
  fprintf(stderr, "Hello world.\n");

  dispatch_queue_t q = dispatch_queue_create("my.queue", DISPATCH_QUEUE_SERIAL);
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);

  global = 44;
  dispatch_data_t data = dispatch_data_create("buffer", 6, q, ^{
    fprintf(stderr, "Data destructor.\n");
    global++;

    dispatch_semaphore_signal(sem);
  });
  dispatch_release(data);
  data = nil;

  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

  data = dispatch_data_create("buffer", 6, q, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  dispatch_release(data);
  data = nil;

  fprintf(stderr, "Done.\n");
}

// CHECK: Hello world.
// CHECK: Data destructor.
// CHECK-NOT: WARNING: ThreadSanitizer
// CHECK: Done.
