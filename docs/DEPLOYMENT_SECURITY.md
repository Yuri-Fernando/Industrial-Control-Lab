# Deployment and risk controls

## Topologies

**Local / WSL:** binary and Python gateway on same Linux host; controller bind=127.0.0.1:15020, HTTP bind=127.0.0.1:8088, optional MQTT external.

**Docker:** services `io-node`, `controller`, `gateway`, `mosquitto`. The field network `fieldnet` connects only `io-node` and `controller` (the gateway and broker cannot reach field I/O); `controlnet` carries supervision. The I/O node publishes no host port; host-bound ports 15020, 8088 and 1883 explicitly bind `127.0.0.1` only. The broker configuration allows anonymous access on the private lab network; do not route or expose this port externally.

## Threat model (selected risks)

- Forged Modbus write could command START/ESTOP or override fault injection. Mitigation for demo: loopback bind; production: OT segmentation, secure gateway, audit/authorization, industrial firewall.
- REST command endpoint has no auth or rate limiting. Mitigation: local-only dashboard. Production: identity, RBAC, CSRF/origin considerations, TLS, audit records, command sequence IDs.
- MQTT broker allows anonymous clients to publish. Mitigation: local-only network; production: ACL, credentials/mTLS and nondefault retained-message policies.
- Modbus TCP home-built subset may contain parsing bugs and only supports a subset. v0.2 adds a 20 000-frame fuzz test and a 16-session quota; still replace with an industry-tested implementation before field use and validate interoperability.
- The remote I/O bench registers (freeze, latency) are test-only and writable by any client on the field network; remove or authenticate them outside the lab.
- Broker failure does NOT stop a healthy C++ controller by itself if the gateway continues refreshing heartbeat; if gateway loses communication to controller, watchdog trips. Separate the safety watchdog from telemetry components for production.
- Docker container processes and ordinary Linux scheduling lack hard real-time guarantees.
- Virtual E-stop is **not an emergency stop device**. Never connect it to machines or claim safety certifications.

## Commissioning gate for real hardware

Requires engineering signoff, independent physical safety circuit, verified process limits, calibrated sensors, failure mode and effects analysis, secure OT network, maintenance and change control, and documented hardware-in-the-loop testing. These are explicitly out of scope.
