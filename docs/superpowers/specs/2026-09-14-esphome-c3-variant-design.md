# ESPHome / WiFi variant on ESP32-C3

**Date:** 2026-09-14
**Status:** Approved design, not yet implemented

## Context

The project today is a native ESP-IDF Zigbee firmware for the ESP32-C6: hand-written
drivers for the LD2410 and LD2450, an I2C light sensor, 27 Zigbee endpoints and a
~1100-line Zigbee2MQTT external converter.

Field experience has been that the sensing works and the *transport* is where the
defects live. Recent examples, all Zigbee plumbing rather than detection: attributes
that only reported once Zigbee2MQTT had run Configure, endpoint lists cached at
pairing so a new attribute needs a re-pair, a zone left stale because one report was
dropped and nothing reconciled it, and manufacturer-specific attributes needing
explicit reports because auto-reporting depended on a configuration that could vanish.

An ESPHome build over WiFi removes that entire layer. This spec describes adding one
as a **second variant alongside** the Zigbee firmware, not as a replacement.

## Goals

- A working presence sensor on an ESP32-C3 devkit over WiFi, integrated with Home
  Assistant through the ESPHome native API.
- Behavioural equivalence for the parts that matter: cross-validated occupancy,
  false-positive rejection, zone occupancy, ambient light.
- No custom C. Everything expressed as ESPHome YAML over official components.
- No changes to the existing Zigbee firmware.

## Non-goals

- Replacing the Zigbee build. Both remain supported and documented.
- Zigbee, or a Zigbee router role. The ESP32-C3 has no 802.15.4 radio, so this is
  physically impossible on that part and is not a trade-off being made.
- Five zones. The official ESPHome `ld2450` component supports three.
- A runtime configuration surface equivalent to the Zigbee config cluster.
- UV index, unless the LTR390 is the chosen light sensor.

## Hardware

### UART constraint

The ESP32-C3 has **two** UART peripherals and both radars need one each at 256000
baud. ESPHome's logger occupies UART0 by default, so the config sets
`logger: baud_rate: 0`, which disables serial logging while leaving logs available
over the API and OTA. This frees both UARTs for the sensors and is simpler than the
C6 firmware's arrangement, which moves the console to USB-Serial-JTAG.

There is no spare UART. Any future serial peripheral would need a different SoC.

### Pinout

Chosen to avoid the C3's strapping pins (GPIO2, GPIO8, GPIO9), its SPI flash pins
(GPIO11-17) and its USB pins (GPIO18, GPIO19).

| Connection | ESP32-C3 pin |
|---|---|
| LD2410 TX -> C3 RX | GPIO4 |
| LD2410 RX -> C3 TX | GPIO5 |
| LD2450 TX -> C3 RX | GPIO6 |
| LD2450 RX -> C3 TX | GPIO7 |
| I2C SDA | GPIO0 |
| I2C SCL | GPIO1 |

Both radars take 5V; the light sensor takes 3V3. Radar TX/RX are 3.3V logic, so no
level shifting is needed. Pin assignments are substitutions and can be moved.

### Light sensor

Selected at compile time by a substitution (`bh1750` or `ltr390`). Both blocks live
in the file; only the chosen one compiles. ESPHome declares I2C devices statically,
so it cannot probe and pick at boot the way the C firmware's `light_sensor` facade
does. Declaring both and letting one fail was rejected: it leaves a permanent error
in the logs and a dead entity in Home Assistant.

## Software

### Components

All official, no external dependencies:

| Component | Provides |
|---|---|
| `ld2410` | `has_target`, `has_moving_target`, `has_still_target`, `moving_distance`, `still_distance`, `moving_energy`, `still_energy`, `detection_distance` |
| `ld2450` | `has_target`, `target_count`, `still_target_count`, `moving_target_count`, three zones each with target counts, zone type Disabled/Detection/Filter |
| `bh1750` *or* `ltr390` | illuminance; UV index on the LTR390 |

### Entity model

Raw component outputs are marked `internal: true`. Home Assistant sees only derived
entities, with one deliberate exception: the raw LD2410 and LD2450 presence sensors
are exposed as diagnostic entities.

That exception matters. In the Zigbee firmware, cross-validation happens *before*
reporting, so when the LD2410 misbehaves there is no way to see what it actually
said. Here suppression is a derived sensor sitting on top of visible inputs, which
makes the same class of problem diagnosable from Home Assistant instead of a serial
console.

Exposed: `Occupancy`, `Moving target`, `Still target`, `Target count`, three zones
with occupancy and target counts, `Illuminance`, and `UV index` when fitted.

### Cross-validation and false-positive rejection

The Zigbee firmware suppresses LD2410 detections when the LD2450 sees no targets:

```c
if (shs_ld2450_target_count == 0) { raw_moving = false; raw_static = false; }
```

The ESPHome equivalent is a template binary sensor:

```yaml
- platform: template
  name: "Occupancy"
  lambda: |-
    if (!id(ld2450_target).state) return false;
    if (id(ld2410_moving).state && id(ld2410_moving_distance).state < NEAR_FIELD_CM) return false;
    if (id(ld2410_still).state  && id(ld2410_still_distance).state  < NEAR_FIELD_CM) return false;
    if (id(ld2410_still).state  && id(ld2410_still_energy).state    < MIN_STILL_ENERGY) return false;
    return id(ld2410_target).state;
  filters:
    - delayed_on: SPIKE_FILTER
    - delayed_off: OCCUPANCY_CLEAR_DELAY
```

The capitalised names are substitutions, written in the real file as ESPHome
substitution references.

Mapping of every mechanism in the firmware:

| Zigbee firmware | ESPHome |
|---|---|
| LD2450 cross-validation in `shs_on_state_change` | lambda over both `has_target` sensors |
| `moving_cooldown`, `occupancy_delay` | `delayed_off` filter |
| `zone_occupancy_delay` | `delayed_off` on the zone binary sensor |
| Gate-0 rejection (the disabled `#if 0` block) | lambda on `moving_distance` / `still_distance` |
| `min_moving_energy`, `min_static_energy` | lambda on `moving_energy` / `still_energy` |
| Interference zones (zone type 3) | native zone `Filter` mode |

Two consequences worth stating. The `delayed_off` filter replaces the firmware's
cooldown timers *and* the zone reconciliation added in v1.1.1 - ESPHome owns that
state machine, so the class of bug where a dropped report desynchronises local and
published state does not exist. And the gate-0 and energy filters, which are compiled
out behind `#if 0` in the firmware, are live here and tunable.

### Tuning

Substitutions at the top of the file: `occupancy_clear_delay`, `spike_filter`,
`near_field_cm`, `min_still_energy`, plus the pins and the light sensor choice.
Changing one means editing a line and pushing OTA - roughly 30 seconds, no cable.

Deliberately not a runtime configuration surface. `delayed_off` is compile-time in
ESPHome, and making delays adjustable at runtime requires scripts and globals. The
Zigbee build's runtime config cluster is the feature that has cost the most debugging
time in this project; reproducing it here would be copying a liability.

## Repository layout

```
esphome/
  shs-presence-c3.yaml     the config
  README.md                wiring, flashing, and tuning for this variant
```

The top-level README gains a section stating that two firmwares exist, with a
comparison table and guidance on which to pick.

## Parity with the Zigbee build

| | Zigbee (ESP32-C6) | ESPHome (ESP32-C3) |
|---|---|---|
| Transport | Zigbee, router role | WiFi |
| Integration | Zigbee2MQTT + custom converter | ESPHome native API |
| Zones | 5 | 3 |
| Light sensor | BH1750 or LTR390, auto-detected | one, chosen at compile time |
| UV index | yes, with LTR390 | yes, with LTR390 |
| Runtime config | config cluster attributes | compile-time substitutions + OTA |
| Custom code | ~7300 lines of C and headers | none |

## Verification

There is no test framework and none is proposed. Verification is:

1. `esphome config` - validates the YAML and substitutions.
2. `esphome compile` - proves it builds for the C3.

Both run in GitHub Actions as a job alongside the existing firmware build, so the
variant cannot silently rot when ESPHome releases a breaking change. This mirrors
what the firmware already gets and is the reason the CI job is in scope rather than
deferred.

Hardware verification is manual and mirrors how the firmware was validated: confirm
both radars stream, confirm occupancy tracks a person and clears, confirm a zone
tracks entry and exit, confirm illuminance reads plausibly.

## Risks

- **ESPHome breaking changes.** Mitigated by the CI compile job, which surfaces them
  as a red build rather than a broken flash.
- **Two UARTs, no spare.** Any future serial peripheral needs a different SoC. Noted
  rather than mitigated.
- **WiFi rather than mesh.** A WiFi sensor drops when the AP does, where a Zigbee
  router participates in a mesh. This is inherent to the choice and is why the Zigbee
  build remains the primary one.
- **The 3-zone cap.** Accepted. An external component (TillFleisch/ESPHome-HLK-LD2450)
  offers polygon zones if this ever becomes limiting, at the cost of a third-party
  dependency.
