# Capture analysis

> Reference moved out of the [README](../../README.md); the text is unchanged.

## Capture analysis

`tools/capture.ps1` writes one PresentMon v1 CSV per run. `tools/analyze.py`
summarizes it per swapchain and can append the Markdown report to a findings
file:

```powershell
python tools\analyze.py --histogram captures\2026-11-05-N1.csv
python tools\analyze.py --histogram --append docs\findings\005-sedici-limiter.md captures\2026-11-05-N1.csv
```

`--histogram` adds the `MsBetweenDisplayChange` distribution in 1 ms bins
(`--bin-ms` changes the width). The histogram is how the two clusters of a
non-uniform pattern become visible; the summary tables alone hide them.

When the CSV contains them, these columns are reported as well. They are
relative proxies, not click-to-photon measurements:

| Column | What it measures | What it does not measure |
|---|---|---|
| `MsUntilDisplayed` | PresentMon's software estimate from the Present call to the display change | input sampling, game simulation, photon emission; not comparable across present modes |
| `MsInPresentAPI` | how long the application blocked inside Present | latency; with the limiter it includes the deliberate wait |
| `MsUntilRenderComplete` | Present call to GPU render completion | display latency; mostly queue depth |
| `MsGPUActive` | GPU busy time for the frame | CPU frame time or latency |
