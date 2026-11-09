# UI verification, 2026-11-09

The redesigned UI was built and tested on ARM64, then installed by replacing
and restarting only the dashboard. The ARM64 build, headless dashboard tests,
undefined-behavior sanitizer run, administration settings tests and Wi-Fi
security tests passed. Native-renderer fixtures were visually reviewed.

Injected input events verified power-button wake/sleep and navigation; live
framebuffer checks verified the active root tab. Minecraft stayed running with
the same process identity. Display preferences and Wi-Fi power-policy processes
were unchanged, and the display was returned to off. During a subsequent
15-second screen-off sample the dashboard recorded zero CPU ticks and zero
context switches, and Wi-Fi reported power saving on. This checks observed
behavior, not total device power or the ergonomics of physical finger input.

The subsequent minimal revision was also built and tested with the sanitizer,
then installed by restarting only the dashboard. Live injected-input checks
verified all four tabs, Log/back, and the three indicators on every visited
screen. Minecraft and the power-policy processes stayed running; the screen
was restored to off. Another 15-second sample showed zero dashboard CPU ticks
and context switches, with Wi-Fi power saving on.

See [UI design and validation instructions](../docs/ui.md) for durable guidance.
Earlier live observations in the [power report](2026-11-08-power-baseline.md)
describe the previous UI.
