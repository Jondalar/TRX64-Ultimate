# T112 — We know whether and how Gideon's ESP32 Wi-Fi recovery (module 1.18) serves TRX64 Ultimate as its safety net

**Status:** DECIDED (2026-10-19, D1/D2): back to Gideon is the safety net; waiting for his release with module 1.18,
then S1/S2 on the board. Nothing to build.
**Repo:** TRX64-Ultimate (safety net of ADR-0057); the ESP32 code is Gideon's (`GideonZ/1541ultimate`, branch
`auto-recovery`, PR #1012, `software/u64ctrl/`).
**Number:** T112 (T111 is the highest number in TRX64U-MultiCore's table, T82 here; IDs are shared between the two repos).
**Milestone:** M2.
**Origin:** Alex, 2026-10-18: "Leg mal im TRX64U (nicht MC!) eine Spec an, dass wir uns das dann näher anschauen"
("create a spec in TRX64U (not MC) so that we look at it more closely later"), after the question whether the original ESP32
firmware gets SSL.
**Depends on:** ADR-0057 (the generic core app replaces Gideon's app; his firmware stays installable as the safety net).

---

## §1 The problem

**Who:** Alex, when a TRX64 Ultimate image on the C64U does not boot; later every user of TRX64 Ultimate.

**What they want:** get back to a working machine without a USB stick and without a JTAG cable.

**What exists (read from the code of `upstream/auto-recovery` on 2026-10-18, not run):**

- With the machine off, holding DOWN for 3 s makes the ESP32 control module (ESP32-S3, 8 MB flash) switch the power
  on and run a recovery (`button_handler.c:95-101, 143-151`; `recovery.c`).
- It reads the FPGA's IDCODE (Xilinx required), joins the Wi-Fi stored in the module's NVS, and asks
  `<server>/v1/recovery?serial=..&idcode=..&esp=1.18`: first the server set in the menu (config item 0x65, RPC
  0x1A/0x1B, stored in NVS), then the built-in list, which is only `https://ultimate64.ddns.net`
  (`recovery_trust.c:14-17`, `recovery_url.c:41-70`).
- The manifest (`key=value`, at most 2048 bytes) names the package, the IDCODE, an optional serial pattern, the
  bitstream and the updater image with size and SHA-256, and is signed with ECDSA P-256. The module accepts only keys
  compiled into `recovery_trust.c`: one key, Gideon's (`recovery_trust.c:23-31`, `recovery_verify.c`, mbedTLS).
- TLS: `esp_http_client` with the full ESP-IDF CA bundle, no certificate pinning, plain `http://` accepted
  (`recovery_url.c:14`). The security is the signature and the hashes, not TLS.
- The bitstream is streamed over JTAG into the FPGA (TMS 8, TDI 9, TDO 10, TCK 11; the pins float otherwise,
  `jtag.c:120-162`); the last 4096 bytes wait for the SHA-256. Then the module holds the CPU in reset over the
  bitstream's virtual JTAG (USER4, GPIO bit 7), writes the updater into board RAM (0x10000-0x04000000, MEMCMD/MEMDATA,
  16 KiB blocks), writes `{start, 0x1571BABE}` at 0xFFF8 and releases the reset; the updater asks before it flashes.
- Gideon's updater flashes the ESP32 (bootloader, partition table, app at 0x0/0x8000/0x10000) only when its version
  is higher than the module's (commit c860209a). The module goes 1.14 -> 1.18. The partition table uses about 3 MB of
  the 8 MB; about 4.9 MB are not partitioned; no OTA.
- Licence: the repository is GPLv3 (`LICENSE.txt`); the new recovery files carry no header of their own.

**Our board today:** ESP32 module V1.14 (console "[core] ESP32 link: ok (V1.14 ESP32 WiFi Brid…)", 2026-10-18).

**Parts:**

- **P1 Back to Gideon.** Does his recovery, unchanged, bring a C64U running a broken TRX64 Ultimate image back to his
  firmware? (His server ships his packages; his bitstream has his virtual JTAG.)
- **P2 Coexistence.** Does module 1.18 disturb TRX64 Ultimate when it is not triggered: our app (unknown RPCs, the
  5 Mbit/s link, `modem_on` refused during a recovery), the USB Blaster on P5 (same JTAG lines), the version rule
  of his updater (a module we flash must not be overwritten or block his updates unintentionally)?
- **P3 Our own recovery.** What would recovering *to TRX64 Ultimate* need: our server, our signing key
  (`recovery_trust.c` + a module rebuild and reflash), and whether our bitstream offers what the module drives after
  the load (virtual JTAG on USER4 with CPU reset and memory writes, our boot ROM honouring 0xFFF8 = 0x1571BABE).
  Our debug JTAG is `designs/c64_basic/jtag_dbg.v`; whether it matches his VJI is not checked.
- **P4 TLS for us.** Is the module's mbedTLS/CA-bundle path something TRX64 Ultimate wants for other uses (the open
  PRs #936 and #997 propose TLS for the app through the ESP32; Gideon, 2026-09-28/29: stabilise in a separate test
  project first, prefer mbedTLS; 2026-10-15 on #997: the architecture is not settled)?

## §2 Scenarios — what proves it is answered

```gherkin
Scenario: P1 back to Gideon
  Given a C64U with ESP32 module 1.18, TRX64 Ultimate in flash, and a broken app at 0x220000
  When  the machine is off and DOWN is held for 3 s
  Then  Gideon's updater starts from RAM and offers to flash his firmware

Scenario: P2 coexistence
  Given module 1.18 and TRX64 Ultimate running
  When  the app boots, uses the network and the menu button, and a JTAG load runs with the Blaster on P5
  Then  nothing differs from module 1.14 (console lines, network, rocker events)
```

| # | Solves | Test | Where |
|---|---|---|---|
| S1 | P1 | board step with Alex (needs module 1.18 on the board) | board |
| S2 | P2 | board step: TRX64 Ultimate boot + JTAG load with module 1.18 | board |
| S3 | P3 | read his VJI (Gideon's bitstream behaviour) against `jtag_dbg.v`; result as a table | code reading |

## §3 Decision

Gideon's recovery, unchanged, is the safety net of ADR-0057 (D2); module 1.18 arrives with his release (D1).

## §4 Out

Building anything; flashing the ESP32 module ourselves; a recovery server, signing key or ESP32 image of our own
(P3, parked by D2); P4 (TLS) is not taken up here.

## §5 Decisions taken while building

- **D1 (Alex, 2026-10-19, Q1): module 1.18 comes with Gideon's release.** "Wir machen A und warten erstmal" ("we do A
  and wait for now"). No module built or flashed from his `auto-recovery` branch; his updater installs 1.18 when a
  release carries it. Until then JTAG is the way back from a broken image. S1/S2 wait for that release.

## Open questions for Alex

1. ~~**Q1** When do we put module 1.18 on the board?~~ Answered: D1, with Gideon's release.
2. ~~**Q2** Is recovery back to Gideon (P1) enough, or do we want our own recovery (P3)?~~ Answered: D2, back to Gideon.
