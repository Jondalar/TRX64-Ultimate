# 7. Timewarp

While the C64 runs, our bitstream records every change of the machine into memory, frame by frame.
You can go back to any recorded frame and continue from there.

How far back depends on what the program does: the ring holds a time window up to a size limit.
`trxmon`'s `rewind` command shows the range actually held.

The ring is off while the REU is on (they share the memory).

## The Timewarp bar

Set **Interface Type** to Freeze, open the menu, then **C64 Machine > Timewarp**. From the
Overlay menu it answers "Timewarp works from the menu over a frozen machine."

![Timewarp bar](img/timewarp-bar.png)

The picture shows the chosen frame; the bar shows how far back it is and the frame number.

| Key | Effect |
|---|---|
| Cursor left / right | One frame back / forward |
| Cursor up / down | One second back / forward |
| F1 / F7 | Ten seconds back / forward |
| HOME | The oldest frame |
| RETURN | Continue from this frame; the menu closes |
| RUN/STOP | Back to the menu; the machine continues where it was frozen |

Continuing from an older frame discards the later frames.

## From the monitor and over the network

`trxmon` has `rewind`, `frame`, `goto`, `rstep` and marks ([6.](06-monitor.md)). REST offers
`/v1/timewarp:status`, `:frame`, `:start`, `:stop`, `:rewind`, `:scrub` and `:scrubend`
([9.](09-network.md)).

The ring cannot be saved to a file in this release.
