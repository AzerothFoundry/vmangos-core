# Current-instance Boss browser

`.gmui tele bosses [page]` requires the existing GM UI administrator permission.
It is read-only, lists the current dungeon map catalog in entry order, eight rows per
page, and never loads a creature or changes an encounter. Names use session DB locale.

Wire rows: `GMUI|BOSS|id=...|name=...|map=...|instance=...|available=0/1|reason=loaded_boss/static_spawn/native_anchor/no_static_spawn`.
Metadata: `GMUI|BOSSMETA|page=...|total=...|more=0/1|map=...|instance=...`, then `GMUI|DONE|bosses`.
An empty list still returns metadata and DONE. Errors return only `GMUI|ERROR|...`.
`available=1` means a loaded Boss or static spawn anchor exists, not that a safe landing is guaranteed.
Dynamically summoned encounters without a loaded actor, native source anchor or static spawn remain visible and unsupported.
No coordinates are invented and no encounter is summoned or forced into a new state.

`.gmui tele boss <entry>` accepts only a catalog entry belonging to the current map.
The command prefers a loaded current-instance Boss position, otherwise a native static spawn, deterministically by GUID. It rejects dead,
in-combat, teleporting, flying, queued or transported players and active instance
encounters. The final call is NearTeleportTo on the current instance, not cross-map entry.

Landing candidates are 10/15/20/25/30/40/55/65/75 yards from the native spawn. Each requires actual map
and vmap data, native map ground height, a same-floor height constraint,
dynamic LOS (including doors), and a complete normal navmesh path from the anchor.
Missing data, incomplete/shortcut paths, collision/LOS failures and unsafe nearby
creatures cause rejection. Unloaded hostile spawn footprints use conservative native level/detection/aggro-rate and model size plus wander radius; loaded hostile living creatures use their native GetAttackDistance and actual positions. Dead and friendly actors do not block staging. Current-map registration snapshots include summons without visiting or loading any cells. The boss and encounter state are never modified.

This is conservative staging, not automatic dungeon clearance or immunity. Small
rooms, crowded corridors, missing collision tiles and closed doors may have no usable
point. Unloaded creatures are checked at native spawn/wander footprints; future
waypoint patrol movement cannot be guaranteed absent. Do not interpret a successful
landing as a permanent safe zone or as a gameplay access prerequisite bypass.

Validation must include a real test account in the intended instance: map/instance
isolation, combat rejection, missing-static-spawn rejection, crowded room failure,
closed-door/absent-data failure and at least one collision/navmesh-validated landing.
Syntax checks alone do not validate runtime terrain or encounter behavior.

Fallback anchors are source-derived only (GmInstanceBossAnchors.h records file/line), with parent-spawn references resolved on the current map. They are staging inputs, never an instruction to summon or activate an encounter.

For isolated native diagnostics, `.gmui tele bosscheck <entry>` uses the same current
instance, encounter and actor guards plus the exact production SafePoint pipeline.
It never teleports, saves, recalls or changes the Boss. It returns four bounded
lines: BOSSCHECK (identity, success, preflight, candidate count), BOSSCHECKREJECT
(rejection counts by stage), BOSSCHECKANCHOR (anchor and first height/block sample),
and BOSSCHECKPATH (first failed path flags/length/endpoint error), then DONE bosscheck.
Preflight 1 means disabled collision queries, 2 missing anchor map/vmap data,
3 loaded Boss combat. Counters identify the first rejecting stage for each candidate;
subsequent stages are not run for rejected candidates. `safe=1` ends at the first
fully validated candidate. Diagnostic lines are separate from normal addon UI rows.

WMO-only collision data: native MapManager::ExistMapAndVMap requires both terrain
`.map` and `.vmap` files, which is inappropriate for staging in WMO-only indoor
instances. Native TerrainInfo::GetHeightStatic explicitly accepts absent terrain
and derives floors from vmap geometry. The staging preflight now uses
GridMap::ExistVMap with the identical coordinate-to-grid mapping, then retains all
actual Map::GetHeight validity/same-floor, liquid, LOS, native navmesh and threat
checks. Missing vmaps are still rejected. The collision-data fixture covers
WMO-only presence, missing vmap and invalid coordinates; it does not simulate
successful live navigation or claim a runtime landing passed.

A Boss pivot-to-destination straight line is a visual preference, not reachability.
Candidates must first have a complete normal native navigation path, valid end and
bounded length. Every consecutive native path segment, including the first, must
then pass dynamic/static LOS at the moving actor native collision-body center.
Closed doors or an obstructed first segment still reject the point. A directly
visible validated destination is preferred; only after the search fails to find
one may the first fully validated corner staging point be used. This does not
remove any landing height, liquid, threat or navigation check. BOSSCHECKPATH adds
`failedSegment` (1-based endpoint index) and `corner` (selected fallback indicator).
Empty paths are rejected without calling PathInfo::Length on an empty container.

Native aggro distance correction: BasicAI::MoveInLineOfSight and
NearestHostileUnitInAggroRangeCheck pass SizeFactor::None to the distance test.
GetAttackDistance is therefore already the center-to-center aggro radius; adding
combat reach and bounding radius again incorrectly excludes legitimate staging
points. Static and loaded threat checks now use native aggro distance plus an
explicit 3-yard staging margin, plus static wander radius where applicable.

Sampling uses 448 bounded candidates: 10–75 yards every 5 yards, 32 directions.
PathInfo::setPathLengthLimit divides its argument by the 2-yard smooth step and
caps point count at 256; it is not an actual path-length measurement. Staging now
allows that native maximum point budget (argument 512), while independently
requiring actual path length at most 320 yards. Complete normal paths, endpoint
accuracy and every segment collision test remain required. Partial paths never
become valid merely because their endpoint is near the candidate.

Ragnaros route reference: native PathInfo::BuildPolyPath marks an invalid start
polygon as NOPATH/shortcut; a lava encounter pivot is not a player walkway.
For map409/entry11502 only, native game_tele467 (ragna3) supplies an independent
player-platform navigation origin. It is resolved at runtime, must remain on the
same map, within75 yards and6 height of the actual Boss anchor, and must have
collision data, a valid native floor within2 yards of its record and safe liquid
classification. The Boss anchor, encounter state, candidate positions and threat
checks remain tied to the actual Boss. No coordinate or encounter is relocated.
Normal complete navigation and every collision segment (including the first) are
still required from this declared platform; no nearest-poly guess, partial-path
acceptance or skipped segment is introduced. Missing/invalid platform data gives
preflight4. BOSSCHECKROUTE separately records source=game_tele/id467 and the actual
validated route coordinates; other encounters report source=boss_anchor/id0.
