# v5.15.35

- Cross-transport BLE output is gated until the client enables TX notifications.
- BLE output waits for a short post-connect settle period before notifying.
- BLE disconnect and CCCD changes clear pending output so stale frames are not replayed.
- `SendBLE` reports a clear error when a client is connected without TX notifications enabled.
- `SendWiFi` requires a recently active portal client rather than only a running access point.
- Explicit send acknowledgements say `queued for` because delivery completes asynchronously.
- BLE notification intervals are slightly relaxed for coexistence stability.
