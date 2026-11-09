# Explicit level-60 PvE companions

New administrator-only, real-player commands:

- `.gmui party specs`: 17 real database specs; PARTYSPEC rows and DONE|party.
  Sixteen have explicit preraid/P6 mappings. Enhancement shaman spec 41 has
  no verified melee P6 gear and is unsupported, never replaced by caster gear.
- `.gmui party add <specId>`: queue a new temporary level-60 PartyBot. Native
  ResetTalents/LearnSpell applies the actual spec; normal PartyBot spell data
  is rebuilt afterward. The real player is never levelled or re-equipped.
- `.gmui party status`: live PARTYBOT rows, PARTYMETA, DONE|partyStatus.

The owner must lead an owner-only group. Other real/offline players or foreign
bots cause refusal. Pending bots reserve group/instance capacity and count
against PartyBot.MaxBots. The final slot requires an existing ready/pending
policy-created P6 healer or a healer request. No automatic extra summons or
kicks occur. Typed warriors and bears and every healer use their verified P6 anchor.
New protection-paladin spec15 uses the explicit hit-first reference revision
described below; it must not be labelled identical to the original DB template.
New bots may receive the required item qualifications; humans never do.

DPS equipment uses an explicit proxy, not simulated combat power. The closest
known-talent/worn-equipment P6 reference for the player's class normalizes each
available graduation slot; empty slots contribute zero and 2H covers offhand.
DPS targets 0.9-1.1 times this ratio, capped at P6. Owner ratio >=0.97 selects
exact corresponding DPS P6. Otherwise candidates come only from the selected
spec's preraid/P6 anchors and existing 19/29/39/49 same-class, same-role gear.
They must share the P6 anchor's item class/subclass/inventory function, fit the
slot, pass ordinary equip checks and stay within its P6 proxy budget. This
preserves role-specific pieces rather than picking highest-iLvl world items.
A discrete pool may not provide an exact match; limited=1 reports this honestly.
Quality and item level are only a proxy; this does not model enchant throughput
or class-specific DPS. Selected source-template enchants are actually applied.

Status fields:

- PARTYBOT: guid decimal, state loading/ready, specId, role, live level,
  strategy phase6/adaptive/native, p6, verified, equipped, ownerRef, live ratio,
  limited, spells=covered/total. A target spell is covered when actually known
  or superseded by a valid higher rank in the same chain. verified requires
  matching live class/level/AI role and full preset coverage. p6 recomputes each
  canonical P6 item ID and permanent enchant each time, not just policy labels.
- PARTYMETA: owned, pending, ready, p6Healers (loaded and verified P6 healer),
  pendingP6Healers (still-loading plans). Relabelling a mage as healer cannot
  count as a trusted healer spec. Existing raw bots remain strategy=native.

Only newly requested temporary bots are configured. Existing bots, real-player
inventory/talents and PvP BotMatch are untouched. No database schema or global
configuration changes are required. Actual tactical behavior remains the
existing PartyBotAI, not a guarantee of boss-mechanic knowledge or optimal play.

## Attribute inspection, not an optimizer

PARTYSTATS is a separate line per live bot: guid, hitPct (percentage points),
weaponSkill (mainhand getter), defense, armor, health (maximum), currentHealth,
form, currentLevel, targetLevel=63, hitNeed, yellowMiss and whiteDwPenalty.
All attributes use current live state, including race, skill mods and current
form. The existing weapon-skill getter ignores held-weapon skill in feral
shapeshifts and uses level*5. form=0 is humanoid, 1 cat, 5 bear, 8 dire bear;
armor/health reported before a bear transformation are not bear-form stats.

The level-63 reference follows SpellCaster::GetMeleeMissChance for a standing
NPC with defense315 and no target auras or spell-specific hit modifiers:

    diff = mainhandSkill - 315
    base = 5 - diff * (diff < -10 ? 0.2 : 0.1)
    effectiveHit = hitPct - (diff < -10 && hitPct > 0 ? 1 : 0)
    yellowMiss = clamp(base - effectiveHit, 0, 60)
    hitNeed = max(0, base + (diff < -10 ? 1 : 0))

Thus skill300 requires9%, skill305 requires6%, skill310 requires5.5% for this
reference. At skill300 and zero +hit, the source formula yields8% yellow miss;
when any positive +hit exists the first1% is ignored. This discontinuity is
preserved, not silently smoothed. The normal dual-wield white-attack19% penalty
is reported separately and never added to yellowMiss. The core may omit that
white penalty while particular melee attacks are queued/cast. This reference
is not a measurement against an actual boss and does not model parry, dodge,
block, glancing blows, spell-hit chance, incoming crit/crush, or encounter
mechanics. No universal defense440 rule is inferred. A complete P6 template
is not proof that these reference thresholds are met or that it is optimal.
No loadout is changed merely to force this reference to zero.

## Protection-paladin hit-first reference revision

Only new typed spec15 companions replace Onyxia Blood Talisman18406 with
the existing legal Drake Fang Talisman19406. Styleen remains unchanged. This
is a local equipment reference choice, not a database edit or hidden hit aura.
Ordinary item checks still apply. Item-text trade: -8defense/-1%parry,
+2%hit/+1%dodge/+56AP; defense itself also changes avoidance, so these are
not promised net character-sheet changes. HP and armor are unchanged by the
item text. This is a reviewed minimal hit-first trade, not a global optimum.

PARTYBOT strategy=tank_hit marks this policy. p6 still compares the original
DB template31 ID+enchant set and therefore can be0; new loadout=1 means the
complete trusted chosen slot/ID/enchant snapshot is actually equipped now.
No plan-only flag is accepted as proof. All other typed chosen sets also get
this separate live loadout check. verified remains the real spec/class/role
check, and healer guarantees still require the original P6 identity.

Each newly initialized tank must pass the level63 yellow-miss reference with
remaining miss <=0.001%, otherwise initialization is rejected with an explicit
error and the temporary companion is removed. A feral tank uses level*5 skill
for this reference, never a held-weapon skill bonus. Later stats are live current
state and can change with debuffs/form; no combat outcome is guaranteed.

PARTYDEFENSE is an independent short line: guid,targetLevel=63,critRef (remaining
default5% ordinary-melee crit reference after current defense),armorDr (current
armor/(armor+5755), clamped0..75%),dodgePct/parryPct/blockPct (current conditional
Unit getters),blockValue,basis=current. This does not model boss facing, spells,
crushing blows or the full attack table. No universal defense440 graduation
requirement is introduced.

## Owner-only removal without target selection

`.gmui party remove all` marks only the exact owner's PartyBot entries for
normal removal, including loading companions. It does not resolve a selected
world player, touch BattleBotAI or other owners, delete a human character, or
perform a global bot purge. OK reports the number of newly marked requests.

Status includes closing entries until the manager completes the normal bot
logout/removal path: PARTYBOT state=removing and PARTYMETA removing=N. They
remain in owned, but are not counted ready/pending or as available P6 healers.
Repeated requests are safe and may report0 while earlier removals finish.
Consumers should poll owned=0 before reporting owner cleanup complete.

The pre-existing AI also requests removal when GetPartyLeader cannot resolve
its original owner or finds group ownership inconsistent. The manager then
removes it from the group, logs out its session and erases custom-bot entries.
This mechanism is source evidence, not by itself proof of an old deletion's
runtime cleanup; a necessary controlled world restart also clears temporary
sessions. No Root or unrelated player is acted upon by the new command.

## Pending lifecycle safety regression

A real burst add/add/healer/status/remove/status test found a null-player
crash: GetRole called IsInDuel for a pending ROLE_HEALER before a Player
existed. The installed binary addresses resolve to GetRole -> SendStatus.
Mage roles short-circuited this code, so ready-only tests did not exercise it.

Plan/status paths now separate metadata from live state. A Player is read only
after PB_STATE_ONLINE, completed AI initialization and an in-world pointer.
Pending/removing rows use planned role/spec fields, with no live spell, gear
or attribute claims. FullSpec/Graduated/Loadout also require readiness;
initialization uses a separately validated local Player. The base pointer
explicitly defaults to nullptr; role/duel helpers guard null defensively.

Cancellation transitions loading entries OFFLINE through the existing manager
teardown before OnSessionLoaded can create an avatar. Normal session disposal
also covers World::AddSession-queued sessions. requestRemoval prevents further
AI initialization/group joining. The real burst regression remains required;
sleeps or skipping pending tests are not substitutes for this validation.
