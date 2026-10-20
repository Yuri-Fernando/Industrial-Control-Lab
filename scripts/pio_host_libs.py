# PlatformIO pre-build hook: host-specific link settings for the native platform.
# Windows/MinGW: Winsock2 + winmm (1 ms timer). The C++ runtime is linked statically; when the toolchain has
# no static winpthread (e.g. some bundled MinGW builds), libwinpthread-1.dll is copied next to program.exe
# so the binaries run without the compiler directory on PATH.
import os
import shutil
import sys

Import("env")  # noqa: F821 (provided by SCons)

if sys.platform.startswith("win"):
    env.Append(LIBS=["ws2_32", "winmm"])  # noqa: F821
    gxx = env.WhereIs("g++") or ""  # noqa: F821
    root = os.path.dirname(os.path.dirname(gxx)) if gxx else ""
    static_pthread = any(os.path.isfile(os.path.join(root, sub, "libwinpthread.a"))
                         for sub in ("lib", os.path.join("x86_64-w64-mingw32", "lib")))
    if static_pthread:
        env.Append(LINKFLAGS=["-static"])  # noqa: F821
    else:
        env.Append(LINKFLAGS=["-static-libgcc", "-static-libstdc++"])  # noqa: F821
        dll = os.path.join(root, "bin", "libwinpthread-1.dll")

        def copy_runtime(target, source, env):  # noqa: ARG001
            if os.path.isfile(dll):
                shutil.copy2(dll, os.path.join(os.path.dirname(str(target[0])), "libwinpthread-1.dll"))

        env.AddPostAction("$BUILD_DIR/${PROGNAME}${PROGSUFFIX}", copy_runtime)  # noqa: F821
