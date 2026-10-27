# Data and Method

The data in `resources/` and how weather-generator uses it (rule 6 of the dasmig standard).

**The shipped catalogue is a placeholder written by `tools/new-library.py`.** Replace it, and this page, before the first release.

## Tiers

| Tier | Rows | What it holds |
| --- | --- | --- |
| `lite` | 5 | Sample entries |

## Columns

`weather.tsv`, keyed by `key`:

| Column | Required | Meaning | Source |
| --- | --- | --- | --- |
| `key` | yes | Stable identifier, used by layers | - |
| `label` | yes | Display label | - |
| `weight` | yes | Relative frequency, `>= 0` | - |

For each column, give the source and method, or "curated" with what it was based on, and how reliable it is.

## Generation Model

One entry is drawn by weight from the `"pick"` stream of the call seed.

## Rebuilding the Data

Derived data is rebuilt by the scripts in `scripts/` (raw downloads stay out of the repository).

## Known Limitations

What the model leaves out.
