"""Подставляет компилятор для окружения [env:native].

На Windows системного gcc обычно нет, и `pio test -e native` падает ещё до
компиляции. При этом PlatformIO почти всегда уже держит у себя mingw-w64,
поставленный вместе с другими платформами, — им и пользуемся.

Линкуемся статически: иначе собранному .exe при запуске нужны libstdc++ и
libgcc из каталога toolchain-gccmingw32, которого нет в PATH.
"""

import os
import platform

Import("env")  # noqa: F821  (SCons подставляет Import в область видимости)

if platform.system() == "Windows":
    bindir = os.path.join(
        os.path.expanduser("~"), ".platformio", "packages", "toolchain-gccmingw32", "bin"
    )

    if os.path.isdir(bindir):
        # Каталог кладём в PATH и дальше зовём компилятор голым именем.
        # Абсолютный путь сюда подставлять нельзя: в нём есть пробел
        # ("C:\Users\Denis Nekoz\..."), а SCons не квотирует значение AR —
        # сборка libUnity.a падает с "'C:\Users\Denis' is not recognized".
        env.PrependENVPath("PATH", bindir)
        env.Replace(
            CC="gcc",
            CXX="g++",
            LINK="g++",
            AR="gcc-ar",
            RANLIB="gcc-ranlib",
        )
        env.Append(LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"])
    else:
        print("native_toolchain: mingw не найден в %s, пробую системный gcc" % bindir)
