# Traccar Server and Home Assistant

The working PoC sends OsmAnd HTTP requests to Traccar Server. Home Assistant uses the **Traccar Server** integration. Traccar owns route history, trip/stop reports and geofences; HA's existing Recorder remains independent.

Server deployment and operational scripts live in the separate homelab project. This guide records the firmware's required server contract and the verified deployment baseline, without copying credentials into this repository.

## HA OS services

1. Take a Home Assistant backup before changing services.
2. Install the community **Traccar app** and official **MariaDB app**. The validated baseline is Traccar app **0.27.0 / Server 6.16.0**, MariaDB **3.0.1**.
3. Add a dedicated `traccar` database and database user. Preserve existing MariaDB databases/users and Recorder settings. Configure Traccar's explicit JDBC driver/URL/user/password; a connection error must not fall back to a new empty database.
4. Enable automatic startup and watchdog/restart behavior. Keep the Traccar web UI local on port **8082**, available from the app's **Open Web UI** button. This app does not provide HA ingress embedding.
5. Create the tracker in Traccar with unique identifier **`pablo`**, matching the board's configured ID. Disable unknown-device registration. Use an administrator for server setup and a separate read-only user limited to this vehicle for HA.

The persistent Traccar override for the validated HA app is `/addon_configs/a0d7b954_traccar/traccar.xml`. Adapt bind addresses and credentials to the installation; never publish the private file.

## Ingestion settings

The validated server uses these values:

| Setting | Value / purpose |
|---|---|
| `protocols.enable` | `osmand` |
| `osmand.address` | HA host's local address, reachable only from the tunnel/required local services |
| `osmand.port`, `osmand.speed` | `5055`, `kmh` |
| `database.registerUnknown` | `false` |
| `status.timeout`, `status.ignoreOffline` | `2400`, `osmand`; allow a 30-minute parked heartbeat |
| `filter.enable`, `filter.zero` | `true`, `true` |
| `filter.past`, `filter.outdated`, `filter.duplicate` | `0`, `false`, `false`; accept original backfill timestamps |
| `server.buffering.threshold` | `0`; do not wait for old backlog before accepting the latest location |
| `server.delayAcknowledgement` | `true`; HTTP success still does not prove transactional durability |
| `report.fastThreshold` | `31536001`; ordered trip/stop queries for the retained interval |
| `logger.level` | `warning`; avoid logging normal position traffic |

Position and event timestamp columns in the deployed MariaDB schema use **`TIMESTAMP(3)`**: `tc_positions.servertime`, `devicetime`, `fixtime`, and `tc_events.eventtime`. Millisecond retention was tested after backup and schema adjustment. Recheck precision after a server upgrade and preserve nullability/defaults when maintaining the schema.

See the [technical specification](../technical-specification.md) for payload fields. A fix uses its original timestamp and actual coordinates; no-fix heartbeats omit coordinates. The firmware starts heartbeat reporting only after its first real position has been accepted.

## Cloudflare Access and Tunnel

The firmware endpoint is **`https://tracker.davidmolnar.dev/ingest`**. The homelab tunnel must expose only that exact path to Traccar's internal HTTP OsmAnd listener. Configure the exact path as `^/ingest$` where the connector uses a regular expression, and return 404 for all other routes. Do not expose the administrative UI or REST API through this hostname.

Protect the hostname with an Access **Service Auth** policy allowing only the dedicated device Service Token. The board sends:

```text
CF-Access-Client-Id: <device client ID>
CF-Access-Client-Secret: <device client secret>
```

The deployed device token is non-expiring by the owner's decision. Its values remain private. The tunnel credential, database password and HA administrator token are separate and never belong on the board. No router port forwarding is needed. The ESP32 validates the HTTPS certificate/hostname and does not follow redirects.

Validate missing/incorrect credentials, alternate paths, unknown IDs and a successful authenticated position. Use an isolated test device for synthetic coordinates, then verify the real `pablo` location separately.

## Home Assistant integration and map

1. Add **Traccar Server** from **Settings → Devices & services → Add integration**.
2. Use the local Traccar server address and a token for the dedicated read-only user with access only to `pablo`.
3. Include custom attributes **`mode`, `network`, `gpsFix`, `fix_time`, `reported_at`** in the integration options.
4. Send the first real position. HA creates tracker devices for server devices with a position. Reload this integration if it was added before the first position and the tracker has not appeared yet.
5. Add the [native dashboard card](../../homeassistant/lovelace/dashboard.yaml), adjusting its entity ID if necessary. The verified entity is **`device_tracker.pablo`**.

`fix_time` identifies the last measured location; `reported_at` identifies communication freshness. The deployed homelab helpers use separate freshness thresholds: two minutes active, forty minutes parked. Satellite counts/HDOP are currently local board diagnostics, not HA sensor payload fields. The map card uses standard HA components and needs no custom frontend cards.

## Retention, backup and verification

The deployed database retains **365 days** of routes/events. An hourly MariaDB event deletes at most **10,000 rows total** per run, events first and positions second. It preserves positions referenced by current device state or remaining events and gives newly received old fixes a processing grace period. Only the Traccar schema is touched.

Daily backup runs at **03:15 Europe/Budapest**. Include database events/routines/triggers and test restoration in an isolated database with the event scheduler disabled. The original deployment verified a 52-table restore including the retention definition and millisecond column precision.

Recheck current-marker ordering after backfill, no-fix location preservation, report behavior and real HA coordinates after changes. The current [PoC record](../poc-status.md) confirms a real panel-to-Traccar-to-HA location; LTE and vehicle power tests remain separate gates.

References: [Traccar app documentation](https://github.com/hassio-addons/app-traccar/blob/v0.27.0/traccar/DOCS.md), [HA Traccar Server](https://www.home-assistant.io/integrations/traccar_server/), [OsmAnd protocol](https://www.traccar.org/osmand/), [Cloudflare service tokens](https://developers.cloudflare.com/cloudflare-one/access-controls/service-credentials/service-tokens/).

## LED / relay output

The **Autó** dashboard also includes `switch.pablo_output`. It sends native Traccar custom commands and follows the reported GPIO23 state. The car-only account remains read-only for editing, with **Limit commands** disabled. See [output setup and the HA package](remote-output.md).
