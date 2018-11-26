// RUN: %clang_tsan %s -o %t -framework Foundation
// RUN: %env_tsan_opts=ignore_interceptors_accesses=1 %run %t 2>&1 | FileCheck %s

#import <Foundation/Foundation.h>

long my_global = 0;

int main(int argc, const char *argv[]) {
  fprintf(stderr, "Hello world.\n");

  dispatch_queue_t queue = dispatch_queue_create("my.queue", DISPATCH_QUEUE_SERIAL);
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);

  NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"temp-gcd-io.%d", getpid()]];

  dispatch_io_t channel = dispatch_io_create_with_path(DISPATCH_IO_STREAM, path.fileSystemRepresentation, O_CREAT | O_WRONLY,
      0666, queue, ^(int error) { });
  dispatch_io_set_high_water(channel, 1);

  NSData *ns_data = [NSMutableData dataWithLength:1000];
  dispatch_data_t data = dispatch_data_create(ns_data.bytes, ns_data.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);

  my_global++;
  dispatch_io_write(channel, 0, data, queue, ^(bool done, dispatch_data_t remainingData, int error) {
    my_global++;
    dispatch_async(queue, ^{
      my_global++;
      if (done) {
        dispatch_semaphore_signal(sem);
      }
    });
  });

  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  my_global++;
  dispatch_io_close(channel, 0);
  channel = dispatch_io_create_with_path(DISPATCH_IO_STREAM, path.fileSystemRepresentation, O_RDONLY,
      0, queue, ^(int error) { });
  dispatch_io_set_high_water(channel, 1);

  my_global++;
  dispatch_io_read(channel, 0, SIZE_MAX, queue, ^(bool done, dispatch_data_t remainingData, int error) {
    my_global++;
    dispatch_async(queue, ^{
      my_global++;
      if (done) {
        dispatch_semaphore_signal(sem);
      }
    });
  });

  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  my_global++;
  fprintf(stderr, "Done.\n");
  return 0;
}

// CHECK: Hello world.
// CHECK-NOT: WARNING: ThreadSanitizer
// CHECK: Done.
