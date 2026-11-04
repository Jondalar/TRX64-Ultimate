# 56. TRX64 Ultimate is the monolith; decoupling moves to MultiCore

Date: 2026-10-16

Status: Accepted (amends [0003](0003-milestones-m1-m2-m3.md): M2 "decoupling from Gideon" leaves this repository); "we stay with Gideon's app" superseded by [0057](0057-generic-core-app-replaces-gideons-app.md) (2026-10-18)

## Context

On 2026-10-13 the multi-core work (M3 boot chain, host and cores) moved to `TREX/TRX64U-MultiCore`. The M2 rows that decouple the firmware from Gideon (own UI, own API, web interface as an app) were still here, although they belong to the same line as the host app and softboot.

## Decision

Alex 2026-10-16: "Basically we are building a monolith firmware here, a clone of Gideon's, but with ring, Timewarp, monitor and now also the SID sockets. We stay with Gideon's app. That is TRX64Ultimate. The rest, the decoupling, softboot and so on, that is MultiCore."

- TRX64 Ultimate = Gideon's core rebuilt as one bitstream, plus ring, Timewarp, monitor and the SID sockets (T37), with Gideon's app as the application.
- T24, T32, T36, T48 and T52 move to `TREX/TRX64U-MultiCore` (IDs unchanged; agreed with the TRX64-MC session).
- T37 goes into integ-m1 (Alex: "T37 can go in too as far as I'm concerned").
- M2 in this repository: the open work on the monolith (Gideon parity, ring/Timewarp/monitor extras, tests).

## Consequences

The five rows leave TODO.md; their specs and detail files stay here as history. [0002](0002-own-api-gideon-rest-frozen.md) (own API as the target) now concerns MultiCore only.

## Links

- [TODO.md](../../TODO.md) (M2 section)
- [0003](0003-milestones-m1-m2-m3.md)
