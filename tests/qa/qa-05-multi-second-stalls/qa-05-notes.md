# QA-05 notes

Tested:
- Without frame cap or anti-lag.
- With a 30 fps frame cap (no anti-lag).
- With anti-lag (no frame cap).

Observations:
- With default settings the generated output was really capped at about 73-74 fps (not related to anti-lag).
- I went through some very demanding areas, so whenever the base frame rate fell under 30 fps, that was not down to Sedici.
- Cyberpunk does not apply frame generation to its menus, so inside a menu the rate goes back to the real fps. Not all games behave the same in menus.
