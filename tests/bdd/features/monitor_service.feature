# U5: the monitor, its breakpoints and its RPC port belong to the firmware.
Feature: The monitor service is up before any app runs

  The service owns the debug block from boot, so a breakpoint holds the machine
  and the RPC port answers with nothing loaded. It needs two things: the debug
  block in the bitstream (CAPS bit 3) and the trx table of the app API, which
  app_api_refresh() builds. Until 2026-10-31 nothing built that table before the
  service looked, so the service stayed down and said "no debug block" while the
  console had just printed CAPS EF.

  Background:
    Given the TRX64 firmware in the emulator
    And nothing in the physical port

  Scenario: The service comes up with no app loaded
    When the console has said "[TRX] monitor service up" or 20 seconds have passed
    Then the IO block reports the debug block
    And the console says "[TRX] monitor service up"
    And the console never says "the monitor service does not start"
