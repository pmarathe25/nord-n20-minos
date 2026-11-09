# MinOS phone UI

The dashboard is a small control surface for this phone's Minecraft server.
It should answer three questions: is the server ready, is the world backed up,
and is the phone connected and healthy?

## Functionality inventory

| Capability | Exposed controls and information | Location |
| --- | --- | --- |
| Server availability | Ready, starting, stopped, stale status, recovery/charging/time/setup holds | Server |
| Join the game | Current Wi-Fi address; configured game port when online | Server |
| Server lifecycle | Start; save and stop; save and restart | Server |
| Players | Online count and names | Server > Players > Online |
| Access | Whitelist membership, add/remove a username, enforcement on/off | Server > Players > Whitelist |
| Game settings | Difficulty, player limit, view and simulation distances; pending-restart notice | Server > Game settings |
| Diagnostics | Recent server messages, full alerts, current operation and last action | Server > Activity; status banner |
| Backups | Create a local snapshot, dated history, latest marker, available storage | World |
| Restore | Select a backup, review the date and replacement consequences, confirm | World > Restore |
| Networking | Current connection/address, scan, connect, disconnect, supported security and signal strength | Phone > Wi-Fi |
| Wi-Fi credentials | Masked entry, show/hide, case and symbols, space, deletion, insertion cursor | Phone > Wi-Fi > Connect |
| Phone health | Battery percentage, external power and charging state, charge limit/resume threshold, temperature, uptime | Phone |
| Display | Brightness, idle sleep duration, power-button wake guidance | Phone |
| Administration address | SSH address and automatic Wi-Fi power-policy health | Phone > Wi-Fi |

Only capabilities backed by the existing services are exposed. Cloud backups,
hidden/WPA3-only/enterprise network setup, firmware flashing, and recovery-hold
overrides are not implemented here. USB world export remains automatic;
consistent copies still require stopping the server first.

## Organization

Three persistent bottom tabs divide tasks by subject:

- **Server** is the default screen. Readiness and the join address come first,
  followed by lifecycle controls and three rows: Players, Game settings, Activity.
- **World** goes directly to backups. History and restore share the same screen,
  so the chosen backup date remains the focus of the confirmation.
- **Phone** combines connectivity and power with the two display preferences.
  Wi-Fi is the only child page.

Child pages have an explicit parent button and keep their parent tab selected.
Lists use visible Previous/Next controls with a page count. Player and Wi-Fi
pages show six entries, backup pages five. Pagination disappears for a single
page and clamps when a list shrinks. There are no hidden swipe-only actions.

## Presentation and interaction

The native 1080 × 2400 DRM renderer uses a dark neutral background, warm white
text, mint selection/primary controls, and restrained amber/red notices. Titles,
status, supporting text and actions have distinct sizes. Flat rounded surfaces
replace the previous beveled controls and decorative pixel art. Whitespace
separates tasks; the old Tools and Settings launcher pages are removed.

Most controls are at least 120 pixels high (40 logical pixels at a 3× preview
scale). Keyboards use ten 90-pixel-wide keys across the display and place entry
actions near the bottom. Status is always named in text as well as color.
Unknown values stay unavailable instead of appearing as zero or healthy.

Stop, restart, restore, access removal, disabling enforcement and enabling an
empty whitelist require explicit confirmation. Existing freshness checks,
process-identity checks and backup/recovery guards remain in the controller.
The selected restore ID stays attached to its hit target and is checked again
against current history. A failed Wi-Fi send leaves the password form open.

The backup screen states that copies are local and may be incomplete while the
world changes. The confirmation explicitly states that unsaved changes may be
missing. This UI does not force saves or change snapshot/restore behavior.

The renderer keeps bounded glyph caching, scanline damage updates, directory
notifications, idle panel power-down and the wake input guard. The existing
display power, sleep, wake, brightness persistence, polling, input loop and
startup implementations are unchanged. No Wi-Fi power policy, charging guard,
supervisor or administration service was modified. Phone telemetry
refreshes periodically; static pages and forms repaint for relevant changes.

## Validation and previews

Build on ARM64 Debian with `sh tools/build-local.sh`. Compile and run the
headless dashboard test using the command in the main README. It uses
`tests/dashboard-fixtures.h` for synthetic process, battery, network and log
data; previews do not display the phone's private data.

The tests exercise all three root destinations, parent-section mapping,
non-overlapping touch targets, multi-page lists, selected names/backup IDs,
shrinking lists, stale-data restrictions, confirmations, keyboard editing,
drag/multitouch cancellation, power-button handling and partial repaint parity.
The existing administration and Wi-Fi security tests validate the unchanged
backend contracts. The ARM64 build, headless dashboard tests, undefined-behavior
sanitizer run, administration settings tests and Wi-Fi security tests passed.

To render review frames without opening DRM, connecting to a network or running
server commands:

```sh
mkdir -p build/ui-preview
/tmp/n20-dashboard-test --screenshots build/ui-preview
```

This writes PPM images for the main screens, forms, confirmations, stopped,
recovery, empty and unavailable states. Review outputs belong under ignored
`build/` and should be discarded when no longer needed. The redesign was built
and visually reviewed using these native-renderer fixtures.

After separate deployment on 2026-11-09, injected input events on the phone
verified power-button wake/sleep and navigation; live framebuffer checks
verified the active root tab. Minecraft stayed running with the same process
identity. Display preferences and Wi-Fi power-policy processes were unchanged,
and the display was returned to off. During a subsequent 15-second screen-off
sample the dashboard recorded zero CPU ticks and zero context switches, and
Wi-Fi reported power saving on. This checks the observed behavior, not total
device power or the ergonomics of physical finger input.

Building does not install, restart services, or change the live world.
