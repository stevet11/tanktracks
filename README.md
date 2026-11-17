# TANK TRACKS
Drive a tank through a city and shoot everything. C + raylib 5.5. Linux and Windows.
Same engine and build setup as ironroad (single `main.c`, no asset files, sound is synthesized).

## Build
```
make && ./tanktracks
```
or `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`. raylib is taken from
`third_party/raylib-5.5` if present, otherwise downloaded.

## Controls
W/S drive, A/D turn, mouse aims the turret (the cannon shell lands on the red ring),
LMB cannon, RMB or Ctrl machine gun, Q/E look around (it swings back when released), C recenters, mouse wheel zooms, R restart, P pause, Esc quit.

## Targets
Houses, skyscrapers (downtown), trees, fences and lamp posts (crushed by driving over them), water towers (they burst), billboards, fuel tanks and
gas pumps (chain explosions), fire hydrants (geysers), mailboxes, and the ironroad cars (Brawler, Hornet,
Crusher) driving around town. Wrecks burn and sometimes blow up.

Buses, box trucks and fuel tankers share the roads with the cars (a tanker is a rolling bomb). Two trains
run east-west right down the middle of the streets at the 3rd and 6th road rows: a locomotive and six wagons.
They flatten cars at crossings and shove your tank aside, but can be shot to pieces; tank cars explode and
chain down the line. Kill the locomotive and the whole train coasts to a halt; kill a wagon and everything
behind it is stranded. Intact wagons are immovable, but wrecks can be shouldered out of the way (slowly).
Wrecked cars, buses, trucks, tankers and enemy tank wrecks can be shoved aside too, and live buses, trucks
and tankers are too heavy to squash: you push them (small cars still get flattened).
Traffic brakes for tanks, trains and wrecks, and turns around if it stays blocked for 10 seconds.

## Enemy tanks
Five red enemy tanks patrol the roads. Within ~110 units they spot you, hunt you down the road grid,
hold at ~70 units and lead their shots. They die to ~3 cannon hits (500 points each); their shells
hurt you much more than your own splash does. Red dots on the minimap, health bars over their turrets.
