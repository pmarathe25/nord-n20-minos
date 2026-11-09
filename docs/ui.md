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
| Players | Online count and names | Players > Online |
| Access | Whitelist membership, add/remove a username, enforcement on/off | Players (default view) |
| Game settings | Difficulty, player limit, view and simulation distances; pending-restart notice | Server, inline |
| Diagnostics | Recent server messages, full alerts, current operation and last action | Server > Log; status banner |
| Backups | Create a local snapshot, dated history, latest marker, available storage | World |
| Restore | Select a backup, review the date and replacement consequences, confirm | World > Restore |
| Networking | Current connection/address, scan, connect, disconnect, supported security and signal strength | Phone, inline |
| Wi-Fi credentials | Masked entry, show/hide, case and symbols, space, deletion, insertion cursor | Phone > Connect |
| Persistent indicators | Battery percentage, charging marker, Wi-Fi connection state, battery temperature | Every screen and form |
| Phone health | External power and charging state | Phone |
| Display | Brightness, idle sleep duration | Phone, inline |
| Administration address | SSH address; alert if Wi-Fi power policy is unavailable | Phone |

Only capabilities backed by the existing services are exposed. Cloud backups,
hidden/WPA3-only/enterprise network setup, firmware flashing, and recovery-hold
overrides are not implemented here. USB world export remains automatic;
consistent copies still require stopping the server first.

View and simulation distance expose 2–32 chunks; there is no additional
12-chunk phone cap. See [game limits and performance guidance](chunk-distances.md).

## Organization

Four persistent bottom tabs divide tasks by subject:

- **Server** is the default screen. Status and the join address come first,
  followed by lifecycle controls, four direct difficulty choices and distance/
  player-limit steppers. A Log button opens recent activity.
- **Players** opens directly to whitelist access, with an Online view alongside.
- **World** goes directly to backups. History and restore share the same screen,
  so the chosen backup date remains the focus of the confirmation.
- **Phone** combines network selection and display preferences on one page.

Only logs, text-entry forms and confirmations open another screen. Logs have a
back button and keep Server selected. Lists use Previous/Next controls with a
page count. Players show six entries, networks three, and backups five.
Pagination disappears for a single
page and clamps when a list shrinks. There are no hidden swipe-only actions.

## Presentation and interaction

The native 1080 × 2400 DRM renderer uses a dark neutral background, warm white
text, mint selection/primary controls, and restrained amber/red notices. Titles,
values and actions have distinct sizes. Flat rounded surfaces
replace the previous beveled controls and decorative pixel art. Whitespace
separates controls. There are no launcher pages, slogans, page subtitles,
help paragraphs, or descriptive navigation rows. Confirmations keep only the
selected item and essential consequences; empty states use short labels.

Most controls are at least 120 pixels high (40 logical pixels at a 3× preview
scale). Keyboards use ten 90-pixel-wide keys across the display and place entry
actions near the bottom. Status is always named in text as well as color.
Unknown values stay unavailable instead of appearing as zero or healthy.

Stop, restart, restore, access removal, disabling enforcement and enabling an
empty whitelist require explicit confirmation. Existing freshness checks,
process-identity checks and backup/recovery guards remain in the controller.
The selected restore ID stays attached to its hit target and is checked again
against current history. A failed Wi-Fi send leaves the password form open.

The backup list is labeled local. Its confirmation says saved files only and
that live copies may be incomplete. This UI does not force saves or change
snapshot/restore behavior.

The renderer keeps bounded glyph caching, scanline damage updates, directory
notifications, idle panel power-down and the wake input guard. The existing
display power, sleep, wake, brightness persistence, polling, input loop and
startup implementations are unchanged. No Wi-Fi power policy, charging guard,
supervisor or administration service was modified. The top bar samples telemetry
at most once per five seconds using the existing awake loop. It repaints only
its changed rows on static pages and forms. It does not add a timer or read
telemetry while asleep. Missing values appear as unavailable, not zero.

## Validation and previews

Build on ARM64 Debian with `sh tools/build-local.sh`. Compile and run the
headless dashboard test using the command in the main README. It uses
`tests/dashboard-fixtures.h` for synthetic process, battery, network and log
data; previews do not display the phone's private data.

The tests exercise all four root destinations, parent-section mapping,
non-overlapping touch targets, multi-page lists, selected names/backup IDs,
shrinking lists, stale-data restrictions, confirmations, keyboard editing,
drag/multitouch cancellation, power-button handling and partial repaint parity.
Indicator tests cover every page and form, changing/unavailable telemetry,
header-only repaint parity, and no reads or added wake deadline while asleep.
The existing administration and Wi-Fi security tests validate the unchanged
backend contracts. Dated test and deployment results belong in
[the verification report](../reports/2026-11-09-ui-verification.md).

To render review frames without opening DRM, connecting to a network or running
server commands:

```sh
mkdir -p build/ui-preview
/tmp/n20-dashboard-test --screenshots build/ui-preview
```

This writes PPM images for the main screens, forms, confirmations, stopped,
recovery, empty and unavailable states. Review outputs belong under ignored
`build/` and should be discarded when no longer needed.

Building does not install, restart services, or change the live world.
