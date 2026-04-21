# NeoErgo Trimode TODO

## In Progress
- [ ] Battery drain test — confirm all indicator thresholds trigger correctly

## Code Quality
- [ ] Move layer indicators (FN, L2, QW) to keymap.c using enum names
- [ ] Move caps lock indicator to keymap.c
- [ ] Move WPM indicator to keymap.c
- [ ] Goal: neoergo_trimode.c = hardware only, keymap.c = user preferences

## Known Bugs
- [ ] Charging indicator briefly flashes green when switching devices

## Future Features
- [ ] Logic analyzer investigation — confirm CH582F UART battery protocol
- [ ] Smooth RGB fade out at 20% instead of instant cut
- [ ] Auto reconnect after extended disconnection
- [ ] Stealth mode — kill all LEDs while board still functions

## Completed
- [x] Fix KC_NXT fall-through into LT(0, KC_BT1)
- [x] Fix battery reporting — BATVOL packet handling in module.c
- [x] Fix LPWR_TIMEOUT — was 2 minutes, now 30 minutes
- [x] Battery level indicators with charging indicator on LED 12
- [x] KC_BAT percentage readout (yellow=50s, red=10s, blue=1s)
- [x] BAT_DBG toggle for live drain monitoring
- [x] WPM indicator with 100 WPM celebration mode
- [x] QWERTY toggle layer
- [x] Add rgb_matrix_indicators_advanced_user hook
