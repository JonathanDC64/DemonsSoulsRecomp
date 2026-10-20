# pgocc.py <mode> <arg> <compiler> <args...>: the C/C++ compiler launcher of a PGO build (tools/pgo/pgo.sh sets it
# as CMAKE_C_COMPILER_LAUNCHER and CMAKE_CXX_COMPILER_LAUNCHER). It adds the profile flags of <mode> right after the
# compiler, so no CMake file carries profile flags, and the flags are part of every compile command ninja hashes: a
# change of mode or of the profile's path recompiles everything.
#
#   gen   <dir>        IR instrumentation (clang-cl):
#                        -fprofile-generate=<dir>             (a run without LLVM_PROFILE_FILE writes there)
#                        -mllvm -runtime-counter-relocation   (continuous mode on Windows needs it)
#                        -mllvm -disable-vp=true              (continuous mode keeps no value profiles)
#                        -mllvm -static-func-full-module-prefix=false   (a static function's profile name holds
#                                                              its file's base name, not the full path, so a
#                                                              profile also fits a checkout in another folder)
#   use   <profdata>   -fprofile-use=<profdata> -mllvm -static-func-full-module-prefix=false
#                      -Wno-profile-instr-unprofiled. A function whose code no longer matches its profile data still
#                      warns ("hash mismatch", -Wbackend-plugin), so the build log counts what the profile has gone
#                      stale on.
#
# Commands under CMakeScratch or CMakeTmp (CMake's try_compile checks, which link a test exe) run unchanged, so a
# configure check never sees the profile flags.
import os
import subprocess
import sys


def main():
    if len(sys.argv) < 4:
        sys.stderr.write('usage: pgocc.py gen|use <dir or profdata> <compiler> <args...>\n')
        return 2
    mode, arg, cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
    if any('CMakeScratch' in a or 'CMakeTmp' in a for a in cmd):
        return subprocess.call(cmd)
    path = os.path.abspath(arg).replace('\\', '/')
    keep_names = ['-mllvm', '-static-func-full-module-prefix=false']
    if mode == 'gen':
        extra = ['-fprofile-generate=' + path, '-mllvm', '-runtime-counter-relocation',
                 '-mllvm', '-disable-vp=true'] + keep_names
    elif mode == 'use':
        if not os.path.isfile(path):
            sys.stderr.write('pgocc.py: no profile at ' + path + '\n')
            return 2
        extra = ['-fprofile-use=' + path] + keep_names + ['-Wno-profile-instr-unprofiled']
    else:
        sys.stderr.write('pgocc.py: unknown mode ' + mode + '\n')
        return 2
    return subprocess.call(cmd[:1] + extra + cmd[1:])


if __name__ == '__main__':
    sys.exit(main())
