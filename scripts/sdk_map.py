"""Keep a linker map so CI can verify the SDK actually used by the firmware."""
Import("env")  # type: ignore[name-defined] # SCons
env.Append(LINKFLAGS=['-Wl,-Map,"${BUILD_DIR}/${PROGNAME}.map"'])  # type: ignore[name-defined]
