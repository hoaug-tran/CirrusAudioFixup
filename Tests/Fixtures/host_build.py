"""Host fixture build modes. Target kernel/SDK checks remain separate."""
import sys


def sanitizer_flags():
    # The maintained Windows MinGW toolchain has no ASan/UBSan runtime. Run
    # every ordinary fixture here; macOS/Linux CI retain the sanitizer pass.
    if sys.platform == "win32":
        print("SKIP ASan/UBSan on Windows; ordinary fixture still required", flush=True)
        # Darwin production logs use LP64 (%lu for size_t); Windows fixtures
        # use LLP64. Preserve these diagnostics without making that host-only
        # ABI difference fail the production state-machine checks.
        return [["-Wno-error=format", "-D_CRT_SECURE_NO_WARNINGS"]]
    return [[], ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]]
