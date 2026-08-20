# DrfGoldTracker

A [Nexus](https://github.com/RaidcoreGG/Nexus) addon for Guild Wars 2 that tracks gold and item
farming in real time, using [DRF](https://drf.rs)'s live drop feed.

Connects to DRF over websocket, accumulates item/currency deltas into a running session total,
and displays them in an ImGui overlay with trading-post/vendor profit estimates, sortable
table and icon-grid views, and configurable filters/favorites.

## Build

Requires [vcpkg](https://github.com/microsoft/vcpkg) (with `VCPKG_ROOT` set in the environment)
and [Ninja](https://ninja-build.org/) on `PATH`. Must be built x64.

```
cmake --preset x64-debug      # or x64-release
cmake --build --preset x64-debug
```

The output DLL lands in `out/build/<preset>/DrfGoldTracker.dll`. To run it, copy the DLL into
`<Guild Wars 2>/addons/DrfGoldTracker/` and launch the game with
[Nexus](https://github.com/RaidcoreGG/Nexus) installed - it's loaded into the live game process
and can't be run standalone.

## Credits

Several pieces of this addon's design - the session-tracking model, automatic reset scheduling, and some UI/display conventions - are ported or adapted from
[Taschenbuch/BlishHud-FarmingTracker](https://github.com/Taschenbuch/BlishHud-FarmingTracker)
(MIT licensed), a Blish HUD module with the same goal for Guild Wars 2. The DRF websocket
protocol itself is undocumented publicly; connecting to it was worked out with that project's
`DrfWebSocketClient.cs` as a reference.
