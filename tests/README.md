Test Plan Notes (Work In Progress)
=================================

Planned Unit / Integration Tests:

1. Wi-Fi Connection Manager
	- MRU ordering after successful connect
	- Retry exhaustion -> AP fallback transition
	- Scan filtering respects CONFIG_OPDI_NET_SCAN_MIN_RSSI
	- Profile add/update/delete round trip in NVS
	- SHA1 migration legacy -> real digest (idempotent)

2. Camera Module (Phase 1 Stub)
	- TODO: Add test_opdi_cam_config_roundtrip.c
	  * Arrange: delete NVS cam namespace (or simulate) -> init -> expect defaults.
	  * Act: modify config (brightness=5, contrast=-3) set_config -> reload via init -> values persist & clamped.
	- TODO: Add test_opdi_cam_snapshot.c
	  * Snapshot size non-zero (< 4KB) and begins with 0xFF 0xD8, ends with 0xFF 0xD9.
	  * Calling snapshot(NULL,0) returns required size.

3. WebSocket Events
	- TODO: Inject fake state transitions and verify frames queued/broadcast (mock hub).

4. Metrics
	- TODO: Verify periodic metrics timer increments connect_attempts and scan_count.

All TODOs above will be implemented after core feature stabilization.

