# The behaviour of the expansion port, as the firmware decides it.
Feature: A cartridge in the physical expansion port

  The emulator serves a .crt in the expansion port the way a board in the port
  would, beside the firmware's own cartridge emulation. Whether the C64 ever
  sees it is the firmware's decision: "Cartridge Preference" hands the bus to
  the inside or to the outside (software/io/c64/c64.cc, ConfigureU64SystemBus).
  The application's own suites always say "Internal", because the cartridge
  tools want the port for themselves - which is why a cartridge appears not to
  start until a scenario says otherwise.

  Background:
    Given the TRX64 firmware in the emulator

  # cart-info's cart_detect is the state of the lines now, not "a board is
  # plugged in": the probe cartridge holds EXROM low for good (0x01), while an
  # EasyFlash that has banked itself out after its start leaves both high.
  Scenario: The bus is left to the port, so the cartridge runs
    Given the setting Cartridge Preference is "Auto"
    And the probe cartridge in the physical port
    When the machine has been running for 12 seconds
    Then cart-info reports "bus_external" as "0x0F"
    And cart-info reports "bus_internal" as "0x00"
    And cart-info reports "mode" as "8K"
    And the C64 screen shows "TRX64 PORT PROBE"

  Scenario: The bus stays inside, so the cartridge is hidden
    Given the setting Cartridge Preference is "Internal"
    And the probe cartridge in the physical port
    When the machine has been running for 12 seconds
    Then cart-info reports "bus_external" as "0x00"
    And cart-info reports "bus_internal" as "0x0F"
    And the C64 screen does not show "TRX64 PORT PROBE"
    And the C64 screen shows "READY."

  # A game is not in this repository: point $TRX64_BDD_CRT at one to run this.
  @needs-game
  Scenario: An EasyFlash game boots from the port
    Given the setting Cartridge Preference is "Auto"
    And the cartridge named by $TRX64_BDD_CRT in the physical port
    When the machine has been running for 45 seconds
    Then cart-info reports "type" as "EasyFlash"
    And the C64 screen is not the BASIC prompt
