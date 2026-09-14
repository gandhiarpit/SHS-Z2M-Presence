# ESPHome / WiFi Variant on ESP32-C3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second firmware variant to this repo — an ESPHome config for an ESP32-C3 devkit that drives the same LD2410 + LD2450 + light sensor hardware over WiFi into Home Assistant, with no custom C.

**Architecture:** A set of ESPHome YAML packages under `esphome/`, combined by two top-level configs that differ only in which light sensor they pull in. The radars and light sensor are official ESPHome components. Occupancy is a `template` binary sensor whose lambda cross-validates the LD2410 against the LD2450 and applies threshold rejection; its delays come from `template` numbers through templated `delayed_on` / `delayed_off` filters, so every tunable is a Home Assistant entity rather than a compile-time constant. A GitHub Actions matrix compiles both variants on every push.

**Tech Stack:** ESPHome (latest), ESP-IDF framework, ESP32-C3, official `ld2410` / `ld2450` / `bh1750` / `ltr390` components, `esphome/build-action@v7.0.0`.

**Spec:** `docs/superpowers/specs/2026-09-14-esphome-c3-variant-design.md`

## Global Constraints

- **No custom C.** Everything is YAML over official components. A lambda inside a template sensor is YAML; a `custom_component` or an external component is not, and is out of scope.
- **No changes to the existing Zigbee firmware.** Nothing under `main/`, `components/`, `zigbee2mqtt/`, `CMakeLists.txt`, `sdkconfig*` or `partitions.csv` may be touched. The only existing files this plan modifies are `.gitignore` and `README.md`.
- **Board:** `esp32-c3-devkitm-1`, framework `esp-idf`.
- **`logger: baud_rate: 0` is mandatory.** The C3 has exactly two UART peripherals and both radars need one each. Leaving the logger on UART0 takes one of them away. Logs remain available over the API (`esphome logs`), just not on the serial pins.
- **Pin map** (avoids the C3's strapping pins GPIO2/GPIO8/GPIO9, its SPI flash pins GPIO11-17, and its USB pins GPIO18/GPIO19):

  | Connection | C3 pin | ESPHome key |
  |---|---|---|
  | LD2410 TX → C3 RX | GPIO4 | `uart_ld2410.rx_pin` |
  | LD2410 RX → C3 TX | GPIO5 | `uart_ld2410.tx_pin` |
  | LD2450 TX → C3 RX | GPIO6 | `uart_ld2450.rx_pin` |
  | LD2450 RX → C3 TX | GPIO7 | `uart_ld2450.tx_pin` |
  | I²C SDA | GPIO0 | `i2c.sda` |
  | I²C SCL | GPIO1 | `i2c.scl` |

- **Both radar UARTs:** `baud_rate: 256000`, `parity: NONE`, `stop_bits: 1`. The `ld2410` docs state parity and stop bits *must* be these values.
- **Three zones.** The official `ld2450` component supports three, not the firmware's five.
- **Tuning defaults** — these exact values:

  | Entity id | Default | Unit |
  |---|---|---|
  | `occupancy_clear_delay` | 15 | s |
  | `spike_filter` | 0.5 | s |
  | `zone_clear_delay` | 0 | s |
  | `near_field_cm` | 0 (off) | cm |
  | `min_still_energy` | 0 (off) | — |

- **Secrets are never committed.** `esphome/secrets.yaml` is gitignored; `esphome/secrets.yaml.example` is the committed template. CI writes a throwaway one.

## Verification: read this before Task 1

**There is no test framework and none is being added.** The gate on every task is `esphome config` (validates the YAML, substitutions and lambdas' syntax) and `esphome compile` (proves it builds for the C3). That is what the spec specifies and it is the whole test suite.

**This machine has no working Python, no `esphome`, no `gh` and no Docker.** So the canonical verification for every task is the GitHub Actions run, watched in the browser at
`https://github.com/gandhiarpit/SHS-Z2M-Presence/actions/workflows/esphome.yml`.

That makes each verify step a push plus a 4-8 minute wait. Two consequences:

1. **Work on a branch, not `main`.** Each task pushes to trigger CI, and a red intermediate commit on `main` is worse than a red one on a branch. Create it once before Task 1:

```bash
git checkout -b esphome-c3-variant
```

2. **If you can install ESPHome locally, do it before starting — it turns a 5-minute loop into a 2-second one** for the `esphome config` half:

```bash
pipx install esphome
```

Where a step below says *"Run: `esphome config …`"*, run it locally if you have it; otherwise push and read the CI log, where the same command runs as the first half of the compile. Both are written out in each verify step.

## File Structure

| File | Responsibility |
|---|---|
| `.github/workflows/esphome.yml` | Compiles both variants on every push. The only verification. |
| `esphome/shs-presence-c3-bh1750.yaml` | Top-level config, BH1750 build. Substitutions + package list only. |
| `esphome/shs-presence-c3-ltr390.yaml` | Top-level config, LTR390 build. Identical but for the light package. |
| `esphome/packages/base.yaml` | Board, framework, logger, WiFi, API, OTA, and the pin substitutions. |
| `esphome/packages/radars.yaml` | Both UARTs, both radar components, their raw entities and their own config entities. |
| `esphome/packages/tuning.yaml` | The five `template` numbers. |
| `esphome/packages/occupancy.yaml` | The derived `Occupancy` and three zone binary sensors. |
| `esphome/packages/light-bh1750.yaml` | I²C bus + BH1750. |
| `esphome/packages/light-ltr390.yaml` | I²C bus + LTR390, including UV index. |
| `esphome/secrets.yaml.example` | Committed template for the gitignored `secrets.yaml`. |
| `esphome/README.md` | Wiring, flashing, tuning for this variant. |
| `.gitignore` | *(modify)* ignore `esphome/secrets.yaml` and build dirs. |
| `README.md` | *(modify)* add the "Two firmwares" section. |

Why packages rather than one file: **ESPHome has no conditional YAML.** A substitution cannot select between two `sensor:` blocks, so the light sensor choice has to be a choice of which file gets built. Splitting the shared parts into packages means the two top-level configs are four lines each and cannot drift apart.

---

### Task 1: CI harness, base config, and the light sensor

This task's deliverable is the thing every later task is verified against: a C3 config that compiles in CI, connects to WiFi, and reports illuminance. Both light variants are built here because the CI matrix needs both to exist, and because a variant that is never compiled is a variant that rots.

**Files:**
- Create: `.github/workflows/esphome.yml`
- Create: `esphome/shs-presence-c3-bh1750.yaml`
- Create: `esphome/shs-presence-c3-ltr390.yaml`
- Create: `esphome/packages/base.yaml`
- Create: `esphome/packages/light-bh1750.yaml`
- Create: `esphome/packages/light-ltr390.yaml`
- Create: `esphome/secrets.yaml.example`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - Substitutions available to every package: `device_name`, `friendly_name`, `ld2410_rx_pin`, `ld2410_tx_pin`, `ld2450_rx_pin`, `ld2450_tx_pin`, `i2c_sda_pin`, `i2c_scl_pin`.
  - Entity id `illuminance` (sensor, lux) — defined by whichever light package is included, so later tasks may reference it in either build.
  - Entity id `uv_index` (sensor) — LTR390 build only. **Nothing outside `light-ltr390.yaml` may reference it**, or the BH1750 build will fail to compile.

- [ ] **Step 1: Create the branch**

```bash
git checkout -b esphome-c3-variant
```

- [ ] **Step 2: Write `esphome/packages/base.yaml`**

```yaml
# Board, connectivity and the pin map. Shared by both light-sensor variants.
#
# Pins are substitutions so a top-level config can override one without
# editing this file. They avoid the C3's strapping pins (GPIO2, GPIO8,
# GPIO9), its SPI flash pins (GPIO11-17) and its USB pins (GPIO18, GPIO19).

substitutions:
  ld2410_rx_pin: GPIO4      # LD2410 TX -> C3 RX
  ld2410_tx_pin: GPIO5      # LD2410 RX -> C3 TX
  ld2450_rx_pin: GPIO6      # LD2450 TX -> C3 RX
  ld2450_tx_pin: GPIO7      # LD2450 RX -> C3 TX
  i2c_sda_pin: GPIO0
  i2c_scl_pin: GPIO1

esphome:
  name: ${device_name}
  friendly_name: ${friendly_name}

esp32:
  board: esp32-c3-devkitm-1
  framework:
    type: esp-idf

# MANDATORY, not a preference. The C3 has exactly two UART peripherals and
# both radars need one each. baud_rate: 0 releases UART0 from the logger.
# Logs still reach `esphome logs` over the network; they just do not appear
# on the serial pins. Raising this above 0 will break one of the radars.
logger:
  baud_rate: 0

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    password: !secret ota_password

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "${device_name} fallback"

captive_portal:
```

- [ ] **Step 3: Write `esphome/packages/light-bh1750.yaml`**

```yaml
# BH1750 ambient light sensor, I2C address 0x23.
# The matching top-level config is shs-presence-c3-bh1750.yaml.

i2c:
  sda: ${i2c_sda_pin}
  scl: ${i2c_scl_pin}
  scan: true

sensor:
  - platform: bh1750
    id: illuminance
    name: "Illuminance"
    address: 0x23
    update_interval: 60s
```

- [ ] **Step 4: Write `esphome/packages/light-ltr390.yaml`**

```yaml
# LTR390 ambient light + UV sensor, I2C address 0x53.
# The matching top-level config is shs-presence-c3-ltr390.yaml.
#
# `light` is lux; `ambient_light` would be raw ALS counts and is not exposed.
# uv_index exists only in this build, so nothing outside this file may
# reference id(uv_index) or the BH1750 build will not compile.

i2c:
  sda: ${i2c_sda_pin}
  scl: ${i2c_scl_pin}
  scan: true

sensor:
  - platform: ltr390
    address: 0x53
    update_interval: 60s
    light:
      id: illuminance
      name: "Illuminance"
    uv_index:
      id: uv_index
      name: "UV index"
```

- [ ] **Step 5: Write `esphome/shs-presence-c3-bh1750.yaml`**

Note the `radars`, `tuning` and `occupancy` package lines are **deliberately absent** — those files do not exist yet and are added in Tasks 2-5. This config must compile as-is at the end of this task.

```yaml
# ESP32-C3 / WiFi variant of the SHS presence sensor - BH1750 build.
#
# Build and flash this one if the light sensor on the board is a BH1750
# (I2C address 0x23). For an LTR390 (0x53), build shs-presence-c3-ltr390.yaml
# instead. The two configs differ only in the light package below.

substitutions:
  device_name: shs-presence-c3
  friendly_name: SHS Presence C3

packages:
  base:  !include packages/base.yaml
  light: !include packages/light-bh1750.yaml
```

- [ ] **Step 6: Write `esphome/shs-presence-c3-ltr390.yaml`**

```yaml
# ESP32-C3 / WiFi variant of the SHS presence sensor - LTR390 build.
#
# Build and flash this one if the light sensor on the board is an LTR390
# (I2C address 0x53). For a BH1750 (0x23), build shs-presence-c3-bh1750.yaml
# instead. The two configs differ only in the light package below.

substitutions:
  device_name: shs-presence-c3
  friendly_name: SHS Presence C3

packages:
  base:  !include packages/base.yaml
  light: !include packages/light-ltr390.yaml
```

- [ ] **Step 7: Write `esphome/secrets.yaml.example`**

```yaml
# Copy to secrets.yaml and fill in. secrets.yaml is gitignored.
#
# api_key must be 32 random bytes, base64-encoded. Generate one at
# https://esphome.io/components/api.html (the docs page shows a generator),
# or with: openssl rand -base64 32

wifi_ssid: "your-ssid"
wifi_password: "your-wifi-password"
api_key: "REPLACE-WITH-A-GENERATED-BASE64-KEY"
ota_password: "choose-an-ota-password"
```

- [ ] **Step 8: Append to `.gitignore`**

Append these lines to the existing `.gitignore` (do not rewrite the file — the ESP-IDF entries above must stay):

```
esphome/secrets.yaml
esphome/.esphome/
esphome/.pioenvs/
esphome/.piolibdeps/
```

- [ ] **Step 9: Write `.github/workflows/esphome.yml`**

```yaml
name: Build ESPHome variant

# Compiles the ESP32-C3 / WiFi variant. Both light-sensor builds are compiled
# on every push so that whichever one is not currently in use cannot rot when
# ESPHome ships a breaking change.
#
# This is the only verification this variant has. There is no test framework:
# `esphome compile` succeeding is what proves the config is valid for the C3.

on:
  push:
    branches: ['**']
    paths:
      - 'esphome/**'
      - '.github/workflows/esphome.yml'
  pull_request:
    paths:
      - 'esphome/**'
      - '.github/workflows/esphome.yml'
  workflow_dispatch:

jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      # Both variants always run, so one failing does not hide the other.
      fail-fast: false
      matrix:
        variant: [bh1750, ltr390]
    steps:
      - uses: actions/checkout@v4

      # The committed config reads WiFi, API and OTA credentials from
      # secrets.yaml, which is gitignored. CI needs a throwaway one purely so
      # the config resolves. None of this is ever flashed to a real device,
      # and the api_key below is a fixed dummy, not a secret.
      - name: Write throwaway secrets
        run: |
          cat > esphome/secrets.yaml <<'EOF'
          wifi_ssid: "ci-build-only"
          wifi_password: "ci-build-only"
          api_key: "T4mJ9vQZ2pR8sX1wN6yB3kL5hC7dF0gA2eU4iO6zP8s="
          ota_password: "ci-build-only"
          EOF

      - name: Compile
        id: build
        uses: esphome/build-action@v7.0.0
        with:
          yaml-file: esphome/shs-presence-c3-${{ matrix.variant }}.yaml
          # Produces an esp-web-tools manifest alongside the binaries, which
          # is what ESPHome Web needs to flash this over USB.
          complete-manifest: true

      - name: Upload firmware
        uses: actions/upload-artifact@v4
        with:
          # The artifact name carries the variant; the path is the output
          # directory the build action names after the device.
          name: shs-presence-c3-${{ matrix.variant }}
          path: ${{ steps.build.outputs.name }}
          if-no-files-found: error
```

- [ ] **Step 10: Validate the config**

Run locally if you have ESPHome:

```bash
esphome config esphome/shs-presence-c3-bh1750.yaml
```

Expected: the fully-substituted config is printed and the command exits 0. `${device_name}` must appear resolved as `shs-presence-c3`, and `logger:` must show `baud_rate: 0`.

If you do not have ESPHome locally, skip to Step 11 — CI runs the same validation as the first phase of the compile.

- [ ] **Step 11: Commit and push**

```bash
git add .github/workflows/esphome.yml esphome/ .gitignore
git commit -m "Add ESPHome C3 variant: CI, base config and light sensors

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git push -u origin esphome-c3-variant
```

- [ ] **Step 12: Verify CI is green**

Open `https://github.com/gandhiarpit/SHS-Z2M-Presence/actions/workflows/esphome.yml` and watch the run.

Expected: two jobs, `build (bh1750)` and `build (ltr390)`, both green, each taking roughly 4-8 minutes. Each produces a downloadable artifact containing `.bin` files and a `manifest.json`.

**If the upload step fails with "no files found":** the build action's output directory is not where this workflow expects. Add a temporary debug step before the upload — `run: ls -R .` — push, read the log to find the directory holding the `.bin` files, correct the `path:`, and remove the debug step. This is the one input in this workflow taken from the action's documented output name rather than from an observed run.

**If the compile fails on `esp-idf`:** check the error names a component, not the framework. If a component genuinely does not support esp-idf, switch `framework: type:` to `arduino` in `base.yaml` and note it in `esphome/README.md`. Do not work around it with custom code.

---

### Task 2: Both radars

**Files:**
- Create: `esphome/packages/radars.yaml`
- Modify: `esphome/shs-presence-c3-bh1750.yaml`, `esphome/shs-presence-c3-ltr390.yaml` (add the `radars:` package line)

**Interfaces:**
- Consumes: the pin substitutions from `base.yaml` (`ld2410_rx_pin`, `ld2410_tx_pin`, `ld2450_rx_pin`, `ld2450_tx_pin`).
- Produces, for Tasks 4 and 5:
  - Binary sensor ids: `ld2410_target`, `ld2410_moving`, `ld2410_still`, `ld2450_target`
  - Sensor ids: `ld2410_moving_distance`, `ld2410_still_distance` (cm), `ld2410_moving_energy`, `ld2410_still_energy` (0-100), `ld2450_target_count`, `zone_1_count`, `zone_2_count`, `zone_3_count`
  - Component ids: `ld2410_radar`, `ld2450_radar` (referenced by the `ld2450` number/select platforms in Task 5)

- [ ] **Step 1: Write `esphome/packages/radars.yaml`**

The distances and energies are exposed as diagnostics rather than marked internal. They are the values `near_field_cm` and `min_still_energy` are compared against in Task 4, and a threshold you cannot watch is a threshold you cannot set.

Per-gate sensitivity thresholds (`g0_move_threshold` … `g8_still_threshold`, 18 entities) are deliberately **not** exposed. They are available from the same `number` platform if near-field ghosting survives the `near_field_cm` knob; adding them is a YAML block, documented in `esphome/README.md` in Task 6.

```yaml
# Both radars. The LD2410 does presence and energy; the LD2450 does target
# tracking and zones, and acts as the arbiter for cross-validation (see
# occupancy.yaml).
#
# parity: NONE and stop_bits: 1 are required by the ld2410 component, not
# defaults being restated.

uart:
  - id: uart_ld2410
    rx_pin: ${ld2410_rx_pin}
    tx_pin: ${ld2410_tx_pin}
    baud_rate: 256000
    parity: NONE
    stop_bits: 1
  - id: uart_ld2450
    rx_pin: ${ld2450_rx_pin}
    tx_pin: ${ld2450_tx_pin}
    baud_rate: 256000
    parity: NONE
    stop_bits: 1

ld2410:
  id: ld2410_radar
  uart_id: uart_ld2410

ld2450:
  id: ld2450_radar
  uart_id: uart_ld2450

binary_sensor:
  # Raw presence is exposed as diagnostic on purpose. In the Zigbee firmware
  # cross-validation happens before reporting, so when the LD2410 misbehaved
  # there was no way to see what it had actually said. Here the suppression
  # sits on top of visible inputs.
  - platform: ld2410
    ld2410_id: ld2410_radar
    has_target:
      id: ld2410_target
      name: "LD2410 presence"
      entity_category: diagnostic
    has_moving_target:
      id: ld2410_moving
      name: "LD2410 moving"
      entity_category: diagnostic
    has_still_target:
      id: ld2410_still
      name: "LD2410 still"
      entity_category: diagnostic

  - platform: ld2450
    ld2450_id: ld2450_radar
    has_target:
      id: ld2450_target
      name: "LD2450 presence"
      entity_category: diagnostic
    has_moving_target:
      id: ld2450_moving
      internal: true
    has_still_target:
      id: ld2450_still
      internal: true

sensor:
  # Distances are cm; energies are 0-100. Both are what the near_field_cm and
  # min_still_energy knobs in tuning.yaml are compared against.
  - platform: ld2410
    ld2410_id: ld2410_radar
    moving_distance:
      id: ld2410_moving_distance
      name: "Moving distance"
      entity_category: diagnostic
    still_distance:
      id: ld2410_still_distance
      name: "Still distance"
      entity_category: diagnostic
    moving_energy:
      id: ld2410_moving_energy
      name: "Moving energy"
      entity_category: diagnostic
    still_energy:
      id: ld2410_still_energy
      name: "Still energy"
      entity_category: diagnostic

  - platform: ld2450
    ld2450_id: ld2450_radar
    target_count:
      id: ld2450_target_count
      name: "Target count"
    still_target_count:
      id: ld2450_still_count
      internal: true
    moving_target_count:
      id: ld2450_moving_count
      internal: true
    zone_1:
      target_count:
        id: zone_1_count
        name: "Zone 1 target count"
    zone_2:
      target_count:
        id: zone_2_count
        name: "Zone 2 target count"
    zone_3:
      target_count:
        id: zone_3_count
        name: "Zone 3 target count"

number:
  # The radar's own settings, persisted in the radar's flash rather than the
  # C3's. Per-gate thresholds are available here too but are not exposed; see
  # esphome/README.md.
  - platform: ld2410
    ld2410_id: ld2410_radar
    timeout:
      name: "LD2410 timeout"
      entity_category: config
    max_move_distance_gate:
      name: "LD2410 max move gate"
      entity_category: config
    max_still_distance_gate:
      name: "LD2410 max still gate"
      entity_category: config

button:
  - platform: ld2410
    ld2410_id: ld2410_radar
    restart:
      name: "LD2410 restart"
      entity_category: config
  - platform: ld2450
    ld2450_id: ld2450_radar
    restart:
      name: "LD2450 restart"
      entity_category: config
```

- [ ] **Step 2: Add the package to both top-level configs**

In `esphome/shs-presence-c3-bh1750.yaml`, change the `packages:` block to:

```yaml
packages:
  base:   !include packages/base.yaml
  radars: !include packages/radars.yaml
  light:  !include packages/light-bh1750.yaml
```

In `esphome/shs-presence-c3-ltr390.yaml`, change it to:

```yaml
packages:
  base:   !include packages/base.yaml
  radars: !include packages/radars.yaml
  light:  !include packages/light-ltr390.yaml
```

- [ ] **Step 3: Validate**

```bash
esphome config esphome/shs-presence-c3-bh1750.yaml
```

Expected: exits 0. In the printed output, two `uart:` entries appear with `baud_rate: 256000`, and `rx_pin`/`tx_pin` are resolved to `GPIO4`/`GPIO5` and `GPIO6`/`GPIO7`.

If you do not have ESPHome locally, go to Step 4 and read the CI log.

- [ ] **Step 4: Commit and push**

```bash
git add esphome/
git commit -m "Add LD2410 and LD2450 to the ESPHome C3 variant

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git push
```

- [ ] **Step 5: Verify CI is green**

Watch `https://github.com/gandhiarpit/SHS-Z2M-Presence/actions/workflows/esphome.yml`.

Expected: both matrix jobs green.

**If validation fails with "name is required":** an entity marked `internal: true` also needs a `name` in the ESPHome version being used. Give the four internal entities (`ld2450_moving`, `ld2450_still`, `ld2450_still_count`, `ld2450_moving_count`) explicit names and keep `internal: true`; the name is then unused but satisfies the schema.

---

### Task 3: The tuning numbers

**Files:**
- Create: `esphome/packages/tuning.yaml`
- Modify: `esphome/shs-presence-c3-bh1750.yaml`, `esphome/shs-presence-c3-ltr390.yaml`

**Interfaces:**
- Consumes: nothing.
- Produces, for Tasks 4 and 5, five number entity ids whose `.state` is a `float`: `occupancy_clear_delay` (s), `spike_filter` (s), `zone_clear_delay` (s), `near_field_cm` (cm), `min_still_energy` (0-100).

- [ ] **Step 1: Write `esphome/packages/tuning.yaml`**

```yaml
# Runtime-adjustable tuning. Every one of these is a Home Assistant entity;
# none of them needs a reflash to change.
#
#   optimistic: true    - the entity itself is the source of truth, so a value
#                         set from Home Assistant takes effect immediately
#   restore_value: true - persisted to the C3's flash across reboots
#   initial_value       - used on first boot, before anything is stored
#
# restore_value writes flash on every change, which suits a knob turned
# occasionally. Do not add anything here that changes every few seconds.

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

  # Debounce on the ON edge: how long presence must hold before it is
  # published, which rejects single-frame spikes.
  - platform: template
    id: spike_filter
    name: "Spike filter"
    optimistic: true
    restore_value: true
    initial_value: 0.5
    min_value: 0
    max_value: 10
    step: 0.1
    unit_of_measurement: s
    mode: box
    entity_category: config

  - platform: template
    id: zone_clear_delay
    name: "Zone clear delay"
    optimistic: true
    restore_value: true
    initial_value: 0
    min_value: 0
    max_value: 300
    step: 1
    unit_of_measurement: s
    mode: box
    entity_category: config

  # Reject LD2410 detections closer than this. 0 disables it, which matches
  # the Zigbee firmware, where the equivalent gate-0 rejection is compiled
  # out behind #if 0. Compare against the "Still distance" diagnostic to
  # pick a value.
  - platform: template
    id: near_field_cm
    name: "Near-field reject"
    optimistic: true
    restore_value: true
    initial_value: 0
    min_value: 0
    max_value: 200
    step: 5
    unit_of_measurement: cm
    mode: box
    entity_category: config

  # Reject still detections weaker than this. 0 disables it. Compare against
  # the "Still energy" diagnostic to pick a value.
  - platform: template
    id: min_still_energy
    name: "Minimum still energy"
    optimistic: true
    restore_value: true
    initial_value: 0
    min_value: 0
    max_value: 100
    step: 1
    mode: box
    entity_category: config
```

- [ ] **Step 2: Add the package to both top-level configs**

In `esphome/shs-presence-c3-bh1750.yaml`:

```yaml
packages:
  base:   !include packages/base.yaml
  radars: !include packages/radars.yaml
  tuning: !include packages/tuning.yaml
  light:  !include packages/light-bh1750.yaml
```

In `esphome/shs-presence-c3-ltr390.yaml`:

```yaml
packages:
  base:   !include packages/base.yaml
  radars: !include packages/radars.yaml
  tuning: !include packages/tuning.yaml
  light:  !include packages/light-ltr390.yaml
```

- [ ] **Step 3: Validate**

```bash
esphome config esphome/shs-presence-c3-bh1750.yaml
```

Expected: exits 0, and the printed `number:` section contains eight entries — the five template numbers here plus the three `ld2410` ones from Task 2. Packages concatenate lists, so both platforms coexist under one `number:` key; if only one set appears, the package merge is wrong.

- [ ] **Step 4: Commit and push**

```bash
git add esphome/
git commit -m "Add runtime tuning numbers to the ESPHome C3 variant

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git push
```

- [ ] **Step 5: Verify CI is green**

Watch the Actions page. Expected: both matrix jobs green.

---

### Task 4: Derived occupancy

The cross-validation and false-positive rejection, as one template binary sensor. This is the task that replaces `shs_on_state_change()` in the firmware.

**Files:**
- Create: `esphome/packages/occupancy.yaml`
- Modify: `esphome/shs-presence-c3-bh1750.yaml`, `esphome/shs-presence-c3-ltr390.yaml`

**Interfaces:**
- Consumes: `ld2450_target`, `ld2410_target`, `ld2410_moving`, `ld2410_still`, `ld2410_moving_distance`, `ld2410_still_distance`, `ld2410_still_energy` (Task 2); `near_field_cm`, `min_still_energy`, `spike_filter`, `occupancy_clear_delay` (Task 3).
- Produces: binary sensor id `occupancy`.

- [ ] **Step 1: Write `esphome/packages/occupancy.yaml`**

Three things about this lambda are load-bearing:

- A template binary sensor's `lambda` runs every loop iteration, so it re-evaluates continuously — there is no polling interval to set.
- Sensor `.state` is `NaN` before the first reading. Every comparison is guarded, because `NaN < x` is false and would silently disable a filter rather than announce a problem.
- The filter lambdas return **milliseconds**, and each is evaluated when its timer *starts*, not continuously. Changing a delay while one is already pending takes effect on the next transition.

```yaml
# Derived occupancy. The raw radar entities feed this; Home Assistant
# automations should key off "Occupancy", not off the diagnostic entities.

binary_sensor:
  - platform: template
    id: occupancy
    name: "Occupancy"
    device_class: occupancy
    lambda: |-
      // The LD2450 is the arbiter: no tracked target means no occupancy,
      // whatever the LD2410 claims. This is the cross-validation the Zigbee
      // firmware does in shs_on_state_change().
      if (!id(ld2450_target).state) return false;

      // Near-field rejection. 0 disables it, matching the firmware, where
      // the equivalent gate-0 block is compiled out behind #if 0.
      float near = id(near_field_cm).state;
      if (!std::isnan(near) && near > 0) {
        if (id(ld2410_moving).state &&
            id(ld2410_moving_distance).state < near) return false;
        if (id(ld2410_still).state &&
            id(ld2410_still_distance).state < near) return false;
      }

      // Still detections below the energy floor are noise. 0 disables it.
      float floor_energy = id(min_still_energy).state;
      if (!std::isnan(floor_energy) && floor_energy > 0) {
        if (id(ld2410_still).state &&
            id(ld2410_still_energy).state < floor_energy) return false;
      }

      return id(ld2410_target).state;
    filters:
      # delayed_on / delayed_off are templatable and take a lambda returning
      # milliseconds, which is what makes the delays runtime-adjustable
      # rather than compile-time constants.
      - delayed_on: !lambda |-
          float s = id(spike_filter).state;
          return std::isnan(s) ? 500 : (uint32_t)(s * 1000);
      - delayed_off: !lambda |-
          float s = id(occupancy_clear_delay).state;
          return std::isnan(s) ? 15000 : (uint32_t)(s * 1000);
```

- [ ] **Step 2: Add the package to both top-level configs**

In `esphome/shs-presence-c3-bh1750.yaml`:

```yaml
packages:
  base:      !include packages/base.yaml
  radars:    !include packages/radars.yaml
  tuning:    !include packages/tuning.yaml
  occupancy: !include packages/occupancy.yaml
  light:     !include packages/light-bh1750.yaml
```

In `esphome/shs-presence-c3-ltr390.yaml`:

```yaml
packages:
  base:      !include packages/base.yaml
  radars:    !include packages/radars.yaml
  tuning:    !include packages/tuning.yaml
  occupancy: !include packages/occupancy.yaml
  light:     !include packages/light-ltr390.yaml
```

- [ ] **Step 3: Validate**

```bash
esphome config esphome/shs-presence-c3-bh1750.yaml
```

Expected: exits 0. Validation resolves the `id()` references, so a typo in any of the seven consumed ids fails here with `Couldn't find ID '<name>'`.

- [ ] **Step 4: Commit and push**

```bash
git add esphome/
git commit -m "Add cross-validated occupancy to the ESPHome C3 variant

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git push
```

- [ ] **Step 5: Verify CI compiles the lambdas**

Watch the Actions page. Expected: both matrix jobs green.

This is the first task where `esphome config` passing is not enough — the lambdas are C++ and only the compile proves they build. A `std::isnan` that does not resolve, or a filter lambda whose return type does not convert to `uint32_t`, fails here and not in validation. If `std::isnan` is not found, replace the three occurrences with bare `isnan(...)`.

---

### Task 5: Zone occupancy and zone configuration

**Files:**
- Modify: `esphome/packages/occupancy.yaml` (append three zone binary sensors)
- Modify: `esphome/packages/radars.yaml` (append the LD2450 zone config entities)

**Interfaces:**
- Consumes: `zone_1_count`, `zone_2_count`, `zone_3_count`, `ld2450_radar` (Task 2); `spike_filter`, `zone_clear_delay` (Task 3).
- Produces: binary sensor ids `zone_1_occupancy`, `zone_2_occupancy`, `zone_3_occupancy`.

The `ld2450` component has **no zone binary sensors** — it publishes zone target *counts* only. Zone occupancy is therefore derived here, the same way global occupancy is.

- [ ] **Step 1: Append the zone binary sensors to `esphome/packages/occupancy.yaml`**

Add these under the existing `binary_sensor:` key, after the `occupancy` entry. The three are identical but for the zone number — there is no loop construct in ESPHome YAML, so they are written out.

```yaml
  # The ld2450 component publishes zone target counts, not zone occupancy,
  # so occupancy is derived. NaN > 0 is false, so a zone reads clear until
  # the radar's first report rather than flapping.
  - platform: template
    id: zone_1_occupancy
    name: "Zone 1 occupancy"
    device_class: occupancy
    lambda: 'return id(zone_1_count).state > 0;'
    filters:
      - delayed_on: !lambda |-
          float s = id(spike_filter).state;
          return std::isnan(s) ? 500 : (uint32_t)(s * 1000);
      - delayed_off: !lambda |-
          float s = id(zone_clear_delay).state;
          return std::isnan(s) ? 0 : (uint32_t)(s * 1000);

  - platform: template
    id: zone_2_occupancy
    name: "Zone 2 occupancy"
    device_class: occupancy
    lambda: 'return id(zone_2_count).state > 0;'
    filters:
      - delayed_on: !lambda |-
          float s = id(spike_filter).state;
          return std::isnan(s) ? 500 : (uint32_t)(s * 1000);
      - delayed_off: !lambda |-
          float s = id(zone_clear_delay).state;
          return std::isnan(s) ? 0 : (uint32_t)(s * 1000);

  - platform: template
    id: zone_3_occupancy
    name: "Zone 3 occupancy"
    device_class: occupancy
    lambda: 'return id(zone_3_count).state > 0;'
    filters:
      - delayed_on: !lambda |-
          float s = id(spike_filter).state;
          return std::isnan(s) ? 500 : (uint32_t)(s * 1000);
      - delayed_off: !lambda |-
          float s = id(zone_clear_delay).state;
          return std::isnan(s) ? 0 : (uint32_t)(s * 1000);
```

- [ ] **Step 2: Append the zone config entities to `esphome/packages/radars.yaml`**

Add these entries under the existing `number:` key (after the `ld2410` platform entry), and add a new top-level `select:` key at the end of the file.

Zone coordinates are in **millimetres**, relative to the radar, with the radar at the origin: x is left/right (negative is left), y is distance in front.

```yaml
  - platform: ld2450
    ld2450_id: ld2450_radar
    presence_timeout:
      name: "LD2450 presence timeout"
      entity_category: config
    zone_1:
      x1: { name: "Zone 1 X1", entity_category: config }
      y1: { name: "Zone 1 Y1", entity_category: config }
      x2: { name: "Zone 1 X2", entity_category: config }
      y2: { name: "Zone 1 Y2", entity_category: config }
    zone_2:
      x1: { name: "Zone 2 X1", entity_category: config }
      y1: { name: "Zone 2 Y1", entity_category: config }
      x2: { name: "Zone 2 X2", entity_category: config }
      y2: { name: "Zone 2 Y2", entity_category: config }
    zone_3:
      x1: { name: "Zone 3 X1", entity_category: config }
      y1: { name: "Zone 3 Y1", entity_category: config }
      x2: { name: "Zone 3 X2", entity_category: config }
      y2: { name: "Zone 3 Y2", entity_category: config }
```

And at the end of the file:

```yaml
select:
  # One zone_type for all three zones, not one each - the LD2450 has a single
  # region-filtering mode. Detection reports targets inside the zones; Filter
  # ignores them, which is how an interference zone is expressed.
  - platform: ld2450
    ld2450_id: ld2450_radar
    zone_type:
      name: "Zone type"
      entity_category: config
```

- [ ] **Step 3: Validate**

```bash
esphome config esphome/shs-presence-c3-bh1750.yaml
```

Expected: exits 0. The printed `number:` section now holds 21 entries — 5 template, 3 `ld2410`, 13 `ld2450` (presence_timeout plus 12 zone coordinates).

- [ ] **Step 4: Commit and push**

```bash
git add esphome/
git commit -m "Add zone occupancy and zone configuration to the ESPHome C3 variant

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git push
```

- [ ] **Step 5: Verify CI is green**

Watch the Actions page. Expected: both matrix jobs green.

---

### Task 6: Documentation

**Files:**
- Create: `esphome/README.md`
- Modify: `README.md` (top level)

**Interfaces:**
- Consumes: everything above.
- Produces: nothing code depends on.

- [ ] **Step 1: Write `esphome/README.md`**

````markdown
# ESPHome / WiFi variant (ESP32-C3)

A second firmware for the same sensor hardware: an ESP32-C3 devkit talking to
Home Assistant over WiFi instead of an ESP32-C6 talking Zigbee. Same radars,
same light sensors, no custom C.

Design notes and the reasoning behind the trade-offs are in
[`docs/superpowers/specs/2026-09-14-esphome-c3-variant-design.md`](../docs/superpowers/specs/2026-09-14-esphome-c3-variant-design.md).

## Which file to build

| Light sensor fitted | I²C address | Build |
|---|---|---|
| BH1750 | 0x23 | `shs-presence-c3-bh1750.yaml` |
| LTR390 | 0x53 | `shs-presence-c3-ltr390.yaml` |

ESPHome declares I²C devices statically and has no conditional YAML, so it
cannot probe and pick at boot the way the C6 firmware does. The two configs
are otherwise identical — both are a short list of the same packages.

UV index exists only in the LTR390 build.

## Wiring

| Connection | C3 pin |
|---|---|
| LD2410 TX → C3 RX | GPIO4 |
| LD2410 RX → C3 TX | GPIO5 |
| LD2450 TX → C3 RX | GPIO6 |
| LD2450 RX → C3 TX | GPIO7 |
| I²C SDA | GPIO0 |
| I²C SCL | GPIO1 |

Both radars take 5V; the light sensor takes 3V3. Radar TX/RX are 3.3V logic,
so no level shifting is needed.

These pins avoid the C3's strapping pins (GPIO2, GPIO8, GPIO9), its SPI flash
pins (GPIO11-17) and its USB pins (GPIO18, GPIO19). To move one, override the
substitution in the top-level config rather than editing `packages/base.yaml`:

```yaml
substitutions:
  device_name: shs-presence-c3
  friendly_name: SHS Presence C3
  ld2410_rx_pin: GPIO10
```

### Serial logging is off, and has to be

`packages/base.yaml` sets `logger: baud_rate: 0`. The C3 has exactly two UART
peripherals and both radars need one each, so the logger cannot keep UART0.

Logs are still available over the network:

```bash
esphome logs shs-presence-c3-bh1750.yaml
```

Turning serial logging back on will break one of the radars.

## Building and flashing

There is no need to build locally. Every push compiles both variants in GitHub
Actions and attaches the firmware plus an `esp-web-tools` manifest as artifacts
— download the one for your variant and flash it from
[ESPHome Web](https://web.esphome.io/) over USB.

To build locally instead:

```bash
cp secrets.yaml.example secrets.yaml   # then fill it in
esphome run shs-presence-c3-bh1750.yaml
```

`secrets.yaml` is gitignored. Generate the API key with `openssl rand -base64 32`.

## Entities

**Use these for automations:**

| Entity | Notes |
|---|---|
| Occupancy | Cross-validated. This is the one you want. |
| Zone 1-3 occupancy | Derived from the zone target counts. |
| Target count | LD2450 tracked targets. |
| Zone 1-3 target count | |
| Illuminance | lux |
| UV index | LTR390 build only |

**Diagnostic** — the raw inputs the occupancy logic sits on top of. Exposed so a
misbehaving radar is visible from Home Assistant rather than only from a serial
console, and so the thresholds below have something to be set against: LD2410
presence / moving / still, LD2450 presence, moving and still distance, moving
and still energy.

## Tuning

Everything below is a Home Assistant entity. None of it needs a reflash.

| Entity | Default | What it does |
|---|---|---|
| Occupancy clear delay | 15s | How long presence holds after the last detection. |
| Spike filter | 0.5s | How long presence must hold before it is published. |
| Zone clear delay | 0s | Same as the above, per zone. |
| Near-field reject | 0 (off) | Ignore LD2410 detections closer than this, in cm. |
| Minimum still energy | 0 (off) | Ignore still detections weaker than this, 0-100. |
| LD2410 timeout | radar default | The radar's own presence hold. |
| LD2410 max move / still gate | radar default | Furthest gate the radar considers. |
| LD2450 presence timeout | radar default | The radar's own presence hold. |
| Zone 1-3 X1/Y1/X2/Y2 | 0 | Zone corners in mm. x is left/right, y is distance in front, radar at the origin. |
| Zone type | Disabled | Disabled, Detection, or Filter — **for all three zones at once**. |

Two behaviours worth knowing:

- A delay is read when its timer *starts*. Changing one while it is already
  counting down applies from the next transition, not the current one.
- The tuning numbers are written to the C3's flash on every change. That suits
  a knob turned occasionally; do not repurpose one as a fast-changing value.

### Setting a near-field reject

Watch the "Still distance" diagnostic with nobody in the room. If it reports a
small non-zero distance, that is the ghost. Set "Near-field reject" just above
it.

### Per-gate sensitivity

Not exposed, to keep the entity list manageable — the LD2410 has nine gates
with a move and a still threshold each, which is 18 more entities. If
near-field ghosting survives the "Near-field reject" knob, add them to the
`ld2410` number platform in `packages/radars.yaml`:

```yaml
    g0_move_threshold:
      name: "Gate 0 move threshold"
      entity_category: config
    g0_still_threshold:
      name: "Gate 0 still threshold"
      entity_category: config
```

## Differences from the Zigbee build

| | Zigbee (ESP32-C6) | ESPHome (ESP32-C3) |
|---|---|---|
| Transport | Zigbee, router role | WiFi |
| Integration | Zigbee2MQTT + custom converter | ESPHome native API |
| Zones | 5 | 3 |
| Light sensor | BH1750 or LTR390, auto-detected at boot | one, chosen by which file you build |
| Runtime config | config cluster attributes | number/select entities in HA |
| Custom code | ~7300 lines of C and headers | none |

The C3 has no 802.15.4 radio, so this variant cannot do Zigbee or act as a mesh
router. A WiFi sensor also drops when the AP does, where a Zigbee router
participates in a mesh. The Zigbee build remains the primary one.
````

- [ ] **Step 2: Add a section to the top-level `README.md`**

Insert this immediately after the top-level README's introduction, before the hardware or wiring sections:

```markdown
## Two firmwares

This repo builds two different firmwares for the same sensor hardware. Pick one.

| | Zigbee (default) | [ESPHome / WiFi](esphome/README.md) |
|---|---|---|
| Board | ESP32-C6 | ESP32-C3 devkit |
| Transport | Zigbee, acts as a router | WiFi |
| Integration | Zigbee2MQTT + the converter in `zigbee2mqtt/` | ESPHome native API |
| Zones | 5 | 3 |
| Source | `main/`, `components/` | `esphome/` |

The Zigbee build is the primary one: it meshes, it does not depend on your WiFi
staying up, and it supports five zones. Choose the ESPHome variant if you have a
C3 rather than a C6, if you would rather not run Zigbee2MQTT, or if you want the
configuration surface to be ordinary Home Assistant entities.
```

- [ ] **Step 3: Commit and push**

```bash
git add README.md esphome/README.md
git commit -m "Document the ESPHome C3 variant

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git push
```

- [ ] **Step 4: Verify CI is green**

Watch the Actions page.

Note: the workflow's `paths:` filter includes `esphome/**`, so `esphome/README.md` triggers a run but a top-level `README.md`-only change would not. Both files are in one commit here, so a run is expected.

- [ ] **Step 5: Open the pull request**

Open `https://github.com/gandhiarpit/SHS-Z2M-Presence/compare/main...esphome-c3-variant` and create the PR, with this body:

```markdown
Adds an ESPHome / WiFi variant for the ESP32-C3 alongside the existing Zigbee
firmware. No changes to `main/`, `components/` or `zigbee2mqtt/`.

Design: `docs/superpowers/specs/2026-09-14-esphome-c3-variant-design.md`
Plan: `docs/superpowers/plans/2026-09-14-esphome-c3-variant.md`

Verified by `esphome compile` in CI for both the BH1750 and LTR390 builds.
Hardware verification is still outstanding — see the checklist below.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

---

## Hardware verification (manual, after Task 6)

CI proves the config compiles. It cannot prove the thing works, and nothing in
this plan does. Run this on real hardware before treating the variant as usable
— it mirrors how the Zigbee firmware was validated.

- [ ] Flash the correct variant from [ESPHome Web](https://web.esphome.io/) using the CI artifact.
- [ ] Device appears in Home Assistant and the entity list matches `esphome/README.md`.
- [ ] `esphome logs` over the network works, confirming `baud_rate: 0` cost nothing.
- [ ] **Both radars stream.** "LD2410 presence" and "LD2450 presence" both react to a person. If one never changes, its UART pins are swapped — this cost a long debugging session on the C6 build, where the symptom was a byte count of zero with no UART error.
- [ ] "Target count" rises and falls as people enter and leave.
- [ ] "Occupancy" tracks a person and clears roughly 15s after they leave.
- [ ] Change "Occupancy clear delay" to 60 from Home Assistant, confirm the clear takes about a minute, then set it back — this is the whole runtime-config claim, tested.
- [ ] Power-cycle the C3 and confirm the tuning values survived.
- [ ] Set Zone 1's corners, set "Zone type" to Detection, confirm "Zone 1 occupancy" tracks entry and exit.
- [ ] "Illuminance" reads plausibly and changes when the room light does.
- [ ] LTR390 build only: "UV index" is non-zero in direct sunlight. It reads zero indoors, which is correct and was mistaken for a fault once already on the C6 build.
