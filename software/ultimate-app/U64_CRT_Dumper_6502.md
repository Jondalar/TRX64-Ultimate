# U64 CRT Dumper — 6502 Approach (OBSOLETE)

This document previously described a 6502-based approach for reading cartridges
from the U64 cart slot. This approach is **obsolete**.

With the MODE_NORMAL fix (`C64_MODE = MODE_NORMAL` after freeze), the FPGA
releases the cart's native GAME/EXROM pins, making all cart types (including
16K ROMH@$A000) accessible via DMA without any 6502 code.

See [U64_CRT_Dumper.md](U64_CRT_Dumper.md) for the current DMA-based implementation.
