# SPEC-09 Web UI screen view, streams, readmem

| | |
|---|---|
| Status | Done; A1–A3 on hardware open |
| Boards | U64, C64U |
| TODO | P1, P2 |
| Source | prkl `6c00b6aa`, `717e8034` |

## Requirements

- **R1** The web UI (served from `/Flash/html`) shows the C64 screen live, read
  through `readmem`, and a panel to start and stop the data streams.
- **R2** `GET /v1/machine:readmem` does not open or close the menu: reading goes
  through `dma_load_raw_buffer` without releasing the host (`c64_subsys.cc:713-742`),
  so a live view does not disturb a user in the menu. Already so in our tree
  (upstream #714): the host is only released when it is not frozen, and a
  client is only set by `freeze()`; prkl's `!rw` change is not needed.
- **R3** Data streams retry ARP resolution instead of giving up on the first miss
  (`data_streamer.cc:204-206`). Done: the probes go out through one socket that
  stays open across the retries (25 × 100 ms). prkl's literal-IP change is not
  taken: lwIP's `dns_gethostbyname` already accepts a dotted quad (`dns.c:1606`).
- **R4** Note T5 (TODO): frozen reads with `C64_DMA_MEMONLY=1` hang with a real
  cartridge; the live view must use the path that works there. Still open with
  T5: the page reads through `readmem`, which takes the frozen path while the
  menu is up.

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | Web page shows the screen, updating, while a program runs | both boards |
| A2 | Menu open on the C64 while the page polls: menu unaffected | both boards |
| A3 | Stream to a host that answers ARP late: stream starts | hardware |

## Tests

`tests/e2e/web/index_test.py`: opening C64 Screen reads `$0400`, `$D800` and
`$D018-$D021` once and draws them, Play polls and Pause stops; Start and Stop
on a Data Streams row reach that stream with the address typed.
