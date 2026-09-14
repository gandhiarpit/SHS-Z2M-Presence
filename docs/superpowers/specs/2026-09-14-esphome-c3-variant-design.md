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

Chosen at build time. ESPHome declares I2C devices statically and has no
conditional YAML, so it cannot probe and pick at boot the way the C firmware's
`light_sensor` facade does. Declaring both and letting one fail was rejected: it
leaves a permanent error in the logs and a dead entity in Home Assistant.

The choice is therefore which of two top-level configs you build. Each is a short
`packages:` list over a shared base, differing only in the light package it pulls
in, and CI compiles both - so the variant you are not using cannot rot unnoticed.

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
entities, with two deliberate exceptions: the raw LD2410 and LD2450 presence
sensors, and the LD2410 distance and energy readings, are exposed as diagnostic
entities.

That exception matters. In the Zigbee firmware, cross-validation happens *before*
reporting, so when the LD2410 misbehaves there is no way to see what it actually
said. Here suppression is a derived sensor sitting on top of visible inputs, which
makes the same class of problem diagnosable from Home Assistant instead of a serial
console.

The distances and energies are exposed for a narrower reason: they are the values
the near-field and still-energy thresholds are compared against. A threshold you
cannot watch is a threshold you cannot set.

Exposed: `Occupancy`, `Moving target`, `Still target`, `Target count`, three zones
with occupancy and target counts, `Illuminance`, and `UV index` when fitted.

Alongside those sit the configuration entities described under Runtime
configuration, all in the `config` entity category so Home Assistant files them
apart from the readings.

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
    float near = id(near_field_cm).state;
    if (near > 0 && id(ld2410_moving).state && id(ld2410_moving_distance).state < near) return false;
    if (near > 0 && id(ld2410_still).state  && id(ld2410_still_distance).state  < near) return false;
    if (id(ld2410_still).state && id(ld2410_still_energy).state < id(min_still_energy).state) return false;
    return id(ld2410_target).state;
  filters:
    - delayed_on: !lambda "return id(spike_filter).state * 1000;"
    - delayed_off: !lambda "return id(occupancy_clear_delay).state * 1000;"
```

Every threshold and delay in that block is a Home Assistant entity, not a
compile-time constant. See Runtime configuration below.

Mapping of every mechanism in the firmware:

| Zigbee firmware | ESPHome |
|---|---|
| LD2450 cross-validation in `shs_on_state_change` | lambda over both `has_target` sensors |
| `moving_cooldown`, `occupancy_delay` | templated `delayed_off` filter |
| `zone_occupancy_delay` | templated `delayed_off` on the zone binary sensor |
| Gate-0 rejection (the disabled `#if 0` block) | lambda on `moving_distance` / `still_distance` |
| `min_moving_energy`, `min_static_energy` | lambda on `moving_energy` / `still_energy` |
| Interference zones (zone type 3) | native `zone_type: Filter`, all zones or none |

Two consequences worth stating. The `delayed_off` filter replaces the firmware's
cooldown timers *and* the zone reconciliation added in v1.1.1 - ESPHome owns that
state machine, so the class of bug where a dropped report desynchronises local and
published state does not exist. And the gate-0 and energy filters, which are compiled
out behind `#if 0` in the firmware, are live here and adjustable from Home
Assistant, defaulting to off so behaviour matches the firmware until they are
turned on.

### Runtime configuration

Every tunable is a Home Assistant entity, adjustable without a reflash. There are
two sources for them.

**From the radar components, at no cost.** The `ld2410` component exposes
`timeout`, `light_threshold`, the max move and still distance gates, and a
move/still threshold pair for each of gates 0-8. The `ld2450` component exposes
`presence_timeout`, the four corner coordinates of each of the three zones, and a
single `zone_type` select (Disabled / Detection / Filter) that applies to all three
at once - the radar has one region-filtering mode, not one per region. These write
through to the radar's own
flash, so they survive a reboot of either the radar or the C3, and zone geometry
becomes a slider in Home Assistant rather than a byte sequence over UART.

**For the derived sensors, template numbers.** `optimistic: true` makes the
entity authoritative, `restore_value: true` persists it to the C3's flash:

```yaml
number:
  - platform: template
    id: occupancy_clear_delay
    name: "Occupancy clear delay"
    optimistic: true
    restore_value: true
    initial_value: 15
    min_value: 0
    max_value: 300
    step: 1
    unit_of_measurement: s
    mode: box
    entity_category: config
```

The delays reach the filters because `delayed_on`, `delayed_off` and
`delayed_on_off` are templatable - each takes a lambda returning milliseconds:

```yaml
filters:
  - delayed_on: !lambda "return id(spike_filter).state * 1000;"
  - delayed_off: !lambda "return id(occupancy_clear_delay).state * 1000;"
```

The thresholds need no templating at all. The occupancy lambda already re-runs on
every input change, so it reads `id(near_field_cm).state` directly.

The full set:

| Entity | Default | Equivalent in the Zigbee build |
|---|---|---|
| Occupancy clear delay | 15s | `occupancy_delay`, `moving_cooldown` |
| Spike filter | 0.5s | debounce on the ON edge |
| Zone clear delay | 0s | `zone_occupancy_delay` |
| Near-field reject | 0cm (off) | gate-0 rejection |
| Minimum still energy | 0 (off) | `min_static_energy` |

Two behaviours to know. A templated filter evaluates its lambda when the timer
*starts*, not continuously, so changing a delay while one is already pending takes
effect on the next transition rather than the current one. And `restore_value`
writes to flash on each change, which suits a knob turned occasionally and would
not suit a value changing every few seconds.

This is the part of the Zigbee build that cost the most debugging time, and it is
worth being clear about why copying it here is not the same bet. The cost there
was never the concept of adjustable settings; it was the transport - attributes
that did not populate until Zigbee2MQTT ran Configure, values that silently
desynchronised from the device after a factory reset, a custom cluster needing
converter code on both sides. ESPHome's number entities are part of the same API
connection as the sensors, so they cannot be configured-but-not-reporting, and
state lives in one place. The liability was the plumbing, not the feature.

Pins and the light sensor choice stay compile-time substitutions. They are wiring
facts, not tuning.

## Repository layout

```
esphome/
  shs-presence-c3-bh1750.yaml   build this one for a BH1750
  shs-presence-c3-ltr390.yaml   build this one for an LTR390
  packages/
    base.yaml                   board, WiFi, API, OTA, logger, pin substitutions
    radars.yaml                 both UARTs, both radars, their entities
    tuning.yaml                 the template numbers
    occupancy.yaml              the derived occupancy and zone sensors
    light-bh1750.yaml
    light-ltr390.yaml
  secrets.yaml.example
  README.md                     wiring, flashing, and tuning for this variant
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
| Runtime config | config cluster attributes | number/select/switch entities in HA |
| Custom code | ~7300 lines of C and headers | none |

## Verification

There is no test framework and none is proposed. Verification is:

1. `esphome config` - validates the YAML and substitutions.
2. `esphome compile` - proves it builds for the C3.

Both run over both top-level configs as a matrix, so the BH1750 and LTR390 variants
are always known to build.

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
