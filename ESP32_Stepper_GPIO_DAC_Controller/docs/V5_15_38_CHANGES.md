# v5.15.38 changes

## Coexistence crash hardening

- Normal Wi-Fi command events now return to Wi-Fi, USB, and AUX UART only.
- Normal BLE command events now return to BLE, USB, and AUX UART only.
- Wi-Fi and BLE cross-routing occurs only through explicit `SendWiFi`,
  `SendBLE`, or `Send` commands.
- BLE notifications are serialized against disconnect and CCCD subscription
  callbacks with a FreeRTOS mutex.
- BLE output waits for connection and notification-subscription settle windows.
- BLE output is discarded rather than submitted to Bluedroid when free heap or
  largest-block headroom falls below the coexistence thresholds.
- Reconnects begin at the current event tail so stale output is not replayed.

## Bounded HTTP requests

- The portal allows at most two active requests.
- Command request bodies and response payloads share a 4,352-byte reservation
  pool.
- Commands use a bounded raw request body instead of URL-encoded form parsing.
- Each command body is owned by its request and freed on disconnect, timeout,
  completion, or abort.
- Bounded JSON replies use the web-server library's request-owned basic response
  instead of a callback response with an extra two-MSS staging buffer.
- Low-heap, fragmented-heap, oversized, and over-capacity requests are aborted
  before another response allocation is attempted.
- Client receive and ACK timeouts prevent abandoned sockets retaining request
  state indefinitely.
- Browser command posts remain serialized and polling pauses during commands.

## Normal Arduino C++ layout

- Replaced implementation `.inc` files with normal `.cpp` files.
- Added shared `ControllerConfig.h` so every translation unit sees the selected
  build profile.
- Grouped framework modules under `src/core`, `src/transports`, `src/radio`,
  `src/web`, `src/hardware`, and `src/util`.
- Isolated the stepper/DAC pack under `src/addons/stepper_dac`.
