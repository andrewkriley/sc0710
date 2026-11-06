# Unattended checks

Run `python3 -B -m unittest discover -s tests -v` with Python 3 and a C
compiler installed. These tests compile functions extracted from the current
driver source, rather than maintaining a separate parser implementation.
Temporary binaries are built outside the checkout and removed afterwards.

The parser runs with AddressSanitizer and UndefinedBehaviorSanitizer, exercising
aligned DMA chunks, every split inside a header, payload splits, and recovery
from inserted garbage. Its sink verifies the order and contents of every line.
It does not test frame assembly, DMA hardware, locking, or physical hot-plug.

Build checks: `make KVERSION=<installed-kernel-version>`. Build different kernel
versions sequentially because they share the `build/` staging directory.
