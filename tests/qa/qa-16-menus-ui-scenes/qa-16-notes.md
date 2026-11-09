# QA-16 notes

## No frame generated in
- Main menu
- Character menu
- In-game menus (settings, graphics and so on)
- Map

In general, everything that stops the main gameplay.

## Important findings
- The loading screen does not run frame generation at first, but once loading finishes it starts doing so.
- Frame generation on the loading screen is apparently caused by the game rendering scenes under it.
- This causes a strange effect: borders show in movement on the loading screens, maybe because the motion vectors work without real input.
