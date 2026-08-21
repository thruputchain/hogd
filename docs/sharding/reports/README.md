# Sharding benchmark reports

Self-contained HTML reports from the sharding benchmark campaign (Aug 2026),
archived here out of the untracked `~/shard-bench/` working directory.
Each was published as a Claude artifact; the URLs are recorded for reference.

## Kept

| File | Artifact | Published |
|---|---|---|
| `sharding-report-2.html` | Sharded vs Unsharded at 50M — `3f8765dc-4948-487e-8742-9f8da72e8daf` | 2026-08-19 |
| `sharding-overview.html` | hogd Database Sharding — `fa1a0ad5-ba07-401a-b901-2da95f9f7881` | 2026-08-18 |
| `sharding-report.html` | Sharding throughput comparison — `f65edc89-65d2-4ed6-8869-666c91f837cb` | 2026-08-15 |

## Superseded

Retained for history; the published artifacts are being retired.

| File | Artifact | Published |
|---|---|---|
| `sharding-vs-not.html` | Does Sharding Help hogd? — `7b4faf80-9712-4550-8094-f01f8d383082` | 2026-08-21 |
| `key-to-shard.html` | How a Key Finds Its Shard — `d392e2fd-7cb6-48e0-98ab-31bbc4fdf3dc` | 2026-08-21 |
| `alice-shard.html` | Where Alice's Data Lives — `0c4a63cc-4022-4249-bdae-97d28f595fd2` | 2026-08-21 |
| `report.html` | never published | 2026-08-12 |

`mkdeck.py` generates `SHARDING-OVERVIEW.pptx` from `sharding-overview.html`.

Raw run data (`results/`, `failed-runs/`, ...) is not archived; it stays in
`~/shard-bench/`.
