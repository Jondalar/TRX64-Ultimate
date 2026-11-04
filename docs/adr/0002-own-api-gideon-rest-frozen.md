# 2. Own API (TRX64 WS JSON-RPC) is the target; Gideon's REST is no longer extended

Date: 2026-10-02

Status: Accepted, amended 2026-10-18 (day 1 of the generic core app: REST first, see below)

## Context

Gideon's app exposes a REST API and web pages; the TRX64 functions (monitor, ring, media/machine/input control) use the TRX64 WebSocket JSON-RPC contract.

## Decision

Decision 2026-10-02 "option 1 as target": stage 2 of the decoupling from Gideon is an own UI (T24, LVGL) and an own API for everything (WS JSON-RPC per the TRX64 contract, also for media/machine/input, web UI on it). Gideon's REST is no longer extended. Gideon's app remains only the base layer.

## Consequences

New functions get JSON-RPC methods, not new REST routes; the command table, the `ult/*` methods and the open questions are in T24-ui-wiring §8, §10. The web interface later becomes a loadable app (T52).

## Links

- [T36](../../docs/specs/T24-ui-wiring.md#t36)
- [T52](../../docs/todo/T52.md)
- [docs/specs/T24-ui-wiring.md](../../docs/specs/T24-ui-wiring.md)

## Amended 2026-10-18

Alex, answer to MultiCore T104.1 Q2 ("1"): on day 1 of the generic core app ([0057](0057-generic-core-app-replaces-gideons-app.md)) the outside API is REST only, through the generic `/v1/ops` adapter plus an alias table for the Gideon REST paths our board tests, deploy scripts and JTAG tools use, so they run unchanged. WS JSON-RPC stays the target and follows after day 1 as a second thin adapter over the same service layer. Gideon's REST code itself is still not extended.
