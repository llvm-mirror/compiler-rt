#!/usr/bin/python

import os, signal, sys, subprocess, tempfile
from android_common import *

ANDROID_TMPDIR = '/data/local/tmp/Output'

here = os.path.abspath(os.path.dirname(sys.argv[0]))
device_binary = os.path.join(ANDROID_TMPDIR, os.path.basename(sys.argv[0]))

def build_env():
    args = []
    # Android linker ignores RPATH. Set LD_LIBRARY_PATH to Output dir.
    args.append('LD_LIBRARY_PATH=%s' % (ANDROID_TMPDIR,))
    for (key, value) in os.environ.items():
        if key in ['ASAN_OPTIONS', 'ASAN_ACTIVATION_OPTIONS']:
            args.append('%s="%s"' % (key, value))
    return ' '.join(args)

is_64bit = (subprocess.check_output(['file', sys.argv[0] + '.real']).find('64-bit') != -1)
asanwrapper = "" if is_64bit else "asanwrapper "

device_env = build_env()
device_args = ' '.join(sys.argv[1:]) # FIXME: escape?
device_stdout = device_binary + '.stdout'
device_stderr = device_binary + '.stderr'
device_exitcode = device_binary + '.exitcode'
ret = adb(['shell', 'cd %s && %s %s%s %s >%s 2>%s ; echo $? >%s' %
           (ANDROID_TMPDIR, device_env, asanwrapper, device_binary, device_args,
            device_stdout, device_stderr, device_exitcode)])
if ret != 0:
    sys.exit(ret)

sys.stdout.write(pull_from_device(device_stdout))
sys.stderr.write(pull_from_device(device_stderr))
retcode = int(pull_from_device(device_exitcode))
# If the device process died with a signal, do abort().
# Not exactly the same, but good enough to fool "not --crash".
if retcode > 128:
  os.kill(os.getpid(), signal.SIGABRT)
sys.exit(retcode)
