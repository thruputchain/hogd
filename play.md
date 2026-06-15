# hog-cluster metrics — operator playbook

Day-2 reference for the Prometheus + Loki + Grafana + node-exporter stack on
`hog1`. The deployment spec is in the ops repo at
`docs/local-hog-metrics-deployment.md`; this file is the short version for
running it.

## Where things live

| Thing | Location |
| --- | --- |
| Host | `hog1` (10.100.0.101) |
| Stage dir | `/home/local/mmayo/hog-cluster-metrics/` on `hog1` local disk. Layout: `docker-compose.yml`, `prometheus/prometheus.yml`, `loki/local-config.yaml`, `grafana/provisioning/{datasources,dashboards}/...`. Each service's config dir is mounted as a directory so edits are picked up live. |
| Config backup | `~/backup/hog-cluster-metrics/` (NFS from hog12 on RAID-1; stage dir + `etc-docker-daemon.json`). Refreshed automatically — see [Backups](#backups). |
| Compose project name | `hog-cluster-monitoring` |
| Persistent data | Docker named volumes `loki_data`, `grafana_data`, `prometheus_data` |
| Docker `data-root` | `/home/local/docker` (relocated from default `/var/lib/docker` so growth lands on the 7.3T RAID). Configured in `/etc/docker/daemon.json`. |
| Volume host paths | `/home/local/docker/volumes/hog-cluster-monitoring_<name>/_data` |
| Ports (bound 0.0.0.0) | Grafana 3000, Prometheus 9090, Loki 3100, node-exporter 9100 |

Configs are off-repo — edit in `/home/local/mmayo/hog-cluster-metrics/`
directly on `hog1`, then trigger a reload per
[Reloading after a config edit](#reloading-after-a-config-edit).

## Access

From an operator workstation:

```bash
ssh -L 3000:hog1:3000 hog1
# then http://localhost:3000 — anonymous Viewer; "Groundhog" folder has the dashboards
```

Or, from any host on `10.100.0.0/24`:

```
http://10.100.0.101:3000
```

Admin user is `admin` / `admin` (local-only, low-stakes).

## Day-2 commands

All run on `hog1`. If you ever get
`permission denied while trying to connect to the docker API`, you're not in
the `docker` group in this shell — wrap with `sg docker -c "..."` or open a
new login session.

```bash
cd /home/local/mmayo/hog-cluster-metrics

# status
docker compose --project-name hog-cluster-monitoring ps

# tail logs of one service
docker compose --project-name hog-cluster-monitoring logs -f prometheus

# restart one sP0+r4B31\ervice (for reload after a config edit, see "Reloading after a config edit" below)
docker compose --project-name hog-cluster-monitoring restart prometheus

# bring the whole stack down / up
docker compose --project-name hog-cluster-monitoring down
docker compose --project-name hog-cluster-monitoring up -d

# pull newer images per the pins in docker-compose.yml
docker compose --project-name hog-cluster-monitoring pull
docker compose --project-name hog-cluster-monitoring up -d
```

## Quick health checks

```bash
curl -sf http://localhost:9090/-/ready                          # Prometheus
curl -sf http://localhost:3100/ready                            # Loki (503 for first ~15s after restart — normal)
curl -sf http://localhost:3000/api/health | jq .                # Grafana
curl -sf http://localhost:9100/metrics | head                   # node-exporter

# Are scrape targets healthy?
curl -sf http://localhost:9090/api/v1/targets \
  | jq '.data.activeTargets[] | {job: .labels.job, node: .labels.node, health, lastError}'

# Spot-check a metric (URL-encode the curly braces!)
curl -sf http://localhost:9090/api/v1/query \
  --data-urlencode 'query=node_load1{job="gh-hog-cluster-host"}' | jq .data.result
```

## Reloading after a config edit

All configs are mounted as **directories** (`./prometheus/`, `./loki/`,
`./grafana/provisioning/`), so the kernel resolves files by name on each
open and edits are picked up immediately. No more inode pinning.

### Prometheus

SIGHUP triggers a reload from disk:

```bash
sg docker -c "docker kill -s HUP hog-cluster-monitoring-prometheus-1"
```

Verify the new config is what Prometheus actually loaded:

```bash
curl -sf http://localhost:9090/api/v1/status/config | jq -r .data.yaml | head -20
```

`POST /-/reload` is not enabled — would need `--web.enable-lifecycle` on
the prometheus command, which means restating its default flag list in
compose. Not worth it; SIGHUP is identical in effect.

### Loki

Loki has no live-reload — restart the container:

```bash
sg docker -c "docker compose --project-name hog-cluster-monitoring \
  -f /home/local/mmayo/hog-cluster-metrics/docker-compose.yml \
  restart loki"
```

### Grafana

Provisioning is rescanned every 30s. Drop or edit a dashboard JSON in
`grafana/provisioning/dashboards/` and wait P0+r4B33\P0+r4B34\P0+r4B35\P0+r6B42\P0+r5053\P0+r5045\— no command needed.

## Fleet scraping

All 12 hogs (`hog1.lan` – `hog12.lan`) run native node-exporter via the
Arch `prometheus-node-exporter` package, managed by
`~/dm/ansible_hog/node-exporter.yml`. The play:

```bash
cd /home/hog12/dm/ansible_hog
ansible-playbook node-exporter.yml -K -e 'ansible_remote_tmp=/tmp/.ansible-mmayo'
```

The `-e ansible_remote_tmp=...` argument is required when running as
`mmayo` (NFS+root_squash; see comment in `~/dm/ansible_hog/`'s commit
history or just keep using the line above). If you ever run as `dm` from
hog12, the `-e` flag is unnecessary.

To add a new host beyond hog1-12 (unlikely):

1. Add it to `[hogs]` in `/home/hog12/dm/ansible_hog/inventory.ini`.
2. Re-run the play above — pacman installs and systemd enables/starts the
   service idempotently.
3. Edit `/home/local/mmayo/hog-cluster-metrics/prometheus/prometheus.yml`
   on hog1 to add another `static_configs` entry under `gh-hog-cluster-host`:
   ```yaml
     - targets: ["hog<n>.lan:9100"]
       labels: { deployment: hog-cluster, node: hog<n> }
   ```
4. SIGHUP Prometheus (see "Reloading after a config edit" above) — no
   restart needed.

The overview dashboard uses `node=~"$node"` so the new hosts appear in the
dropdown automatically.

## Adding log shipping (later)

The logs dashboard is provisioned but empty until something pushes to Loki.
Natural fit on-prem: Vector or Promtail on each hog tailing `journald` and
Docker logs, shipping to `http://hog1:3100` with labels
`job="hog-cluster"`, `deploy="hog-cluster"`, `node="<hostname>"`.

There's a starting-point Vector config at `plg-docker/configs/vector.toml`
in the ops repo.

## Troubleshooting

- **Loki `/ready` returns 503 right after restart** — wait 15s, that's the
  ingester initial-delay. Persistent 503 → check `docker logs ... loki-1`
  for WAL or filesystem errors.
- **PromQL queries return empty when called via curl** — shell ate the
  curly braces. Use `--data-urlencode 'query=...'`.
- **Dashboards present but panels empty** — likely no recent data for the
  selected `$node`/`$deployment`. Check the variable dropdowns in Grafana
  and compare to `curl ... /api/v1/targets`.
- **Container restart-looping after a config edit** — `docker logs
  hog-cluster-monitoring-<service>-1` will usually show the parse error.
  Worst case, revert the file and `docker compose ... up -d`.

## Backups

### Configs (automatic, daily)

A systemd user timer mirrors the stage dir and `/etc/docker/daemon.json`
into `~/backup/hog-cluster-metrics/` every night at ~02:30. The script,
service, and timer live at:

- `~/backup/hog-cluster-metrics/backup.sh`
- `~/.config/systemd/user/hog-cluster-metrics-backup.service`
- `~/.config/systemd/user/hog-cluster-metrics-backup.timer`

Lingering is enabled (`loginctl enable-linger mmayo`) so the timer runs
even when you're not logged in.

```bash
# When is the next run?
systemctl --user list-timers hog-cluster-metrics-backup.timer

# Run now (e.g. right after editing a config)
systemctl --user start hog-cluster-metrics-backup.service

# Tail the backup log
tail -f ~/backup/hog-cluster-metrics/backup.log
```

What's covered:
- All files under `/home/local/mmayo/hog-cluster-metrics/` — compose,
  prometheus, loki, grafana datasources, and all **provisioned**
  dashboard JSONs. New dashboards added by dropping JSON files here are
  automatically picked up by the next run.
- `/etc/docker/daemon.json` (the `data-root` setting).

What's **not** covered:
- The grafana_data volume (`grafana.db`). With `allowUiUpdates: false` in
  `dashboards.yml` this stays effectively empty, so nothing important
  lives there. If you ever flip that flag and start editing in the UI,
  back up the volume too — see "Volume snapshots" below.
- TSDB / Loki chunks. These are observability data, not config, and
  rebuild themselves once scraping resumes.

### Volume snapshots (manual)

The named volumes hold all retained data. If you ever want to snapshot:

```bash
sudo tar -C /home/local/docker/volumes -czf hog-cluster-metrics-volumes.tgz \
  hog-cluster-monitoring_loki_data \
  hog-cluster-monitoring_grafana_data \
  hog-cluster-monitoring_prometheus_data
```

Retention is at defaults; if disks fill, set
`limits_config.retention_period` (Loki) and `--storage.tsdb.retention.time`
(Prometheus) and restart.
