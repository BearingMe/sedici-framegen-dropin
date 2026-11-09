# QA-12 notes

## Settings
- RTSS off.
- Initial log level 0 set in `sedici.toml`.
- Every other change was made through the overlay.
- Ran Cyberpunk's own benchmark.

## Benchmark
| Run | Log level | Overlay | Mean fps | Min fps |
| --- | --- | --- | --- | --- |
| 1 | errors | off | 105.71 | 67.69 |
| 2 | normal | on | 124.19 | 100.07 |
| 3 | errors | off | 122.98 | 105.20 |
| 4 | normal | on | 110.72 | 70.89 |

The Cyberpunk benchmark is unreliable.

## Notes
- The game's fps counter does not match the overlay counter for input.
- The Steam fps counter matches the overlay counter for output.
- Input fps did not settle at 38 during this benchmark (in game about 100, overlay about 60).
