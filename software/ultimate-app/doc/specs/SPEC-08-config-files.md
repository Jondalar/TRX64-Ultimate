# SPEC-08 Assembly64 servers and branding from /Flash

| | |
|---|---|
| Status | Draft |
| Boards | U64, C64U |
| TODO | S11, S12 |
| Source | Spiffy `d9ef6770`, `683ed3c5` |

## Requirements

### Assembly64 servers

- **R1** An optional `/Flash/config/server.json` lists Assembly64 servers (name, host,
  port, client id, URLs), in Spiffy's format (their `config/server.json`).
- **R2** Without the file, behaviour is as today (built-in server, runtime client id,
  `assembly.h:25`).
- **R3** Our `assembly.cc` has diverged from Spiffy's base: the change is written
  against ours.

### Branding

- **R4** An optional `/Flash/config/branding.json` sets the title text, logo and the
  two logo colours, in Spiffy's format.
- **R5** Our logo header is per theme (`userinterface.cc:756-783`): branding applies
  to the theme's header, the TREX theme included (SPEC-01).
- **R6** Without the file, nothing changes.

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | Two servers in server.json appear and both search | hardware (network) |
| A2 | branding.json changes the title and colours in both interfaces | emulator |
| A3 | Malformed JSON: ignored, a line in the log, defaults used | emulator |

## Open questions

- **Q1** Logo glyphs $14–$1F on the U64 overlay font: present? (not checked)
