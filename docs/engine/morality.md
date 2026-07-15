# Morality: the behaviour ledger

## What it is

The first trace of a character's **moral history**. Personality is who a colonist
*is* (innate, and only slowly drifting — see [NPC behaviour](npc-behaviour.md) and the
[Drift](#drift-deeds-reshape-character) section below); morality is what they have *done*
(earned, directly accumulated). It starts as one component and one recorded deed:

- **`BehaviorLedger`** — a per-character tally of six deed **dimensions** (Violence,
  Honesty, Loyalty, Charity, Cruelty, Valor), each an accumulator.
- **`record_deed(actor, kind, mag)`** — the single write-point every deed funnels
  through. It lazily gives the actor a ledger the first time they act.
- **`standing(ledger)`** — one derived scalar the six dimensions collapse to:
  positive is heroic repute, negative is villainous.

**Four** deeds are wired now. Three are hero signals: completing a **rescue** credits the
rescuer with **Charity**; rescuing someone the rescuer was **already bonded to** adds
**Loyalty** on top (standing by your own is more than charity to a stranger); and landing the
**killing blow on a hostile** credits the attacker with **Valor**. The fourth is the first
**villain** signal, and the first deed that pushes `standing` *below zero*: a **player who cuts
down a peaceful colonist** earns **Cruelty** — and if that blow *kills*, **Violence** on top, the
escalation from harm to death. Only **Honesty** now waits for its deed (a truth/deceit event).

## Why it matters

The design's morality is *emergent*, not assigned: nobody is flagged "hero" or
"villain" — you become one by what you repeatedly do, and the world reads that off
your `standing`. That needs three things to be right from the first line, because
they are painful to retrofit: a **single write-point** (so every future deed is one
call, never new plumbing), a **derived** standing (recomputed, never a stored number
that can drift out of sync with the deeds behind it), and a **locked schema** (the
six dimensions the design names, so later deeds append a *value*, never a *field*).

This seed does all three on a real gameplay event, and closes a loop with
personality: the **compassion** axis already decides how *fast* a colonist sprints to
a fallen ally; finishing that rescue now leaves a permanent **moral trace**.

## How it works

Each deed is one call to `record_deed`, which does a single thing:

```cpp
void record_deed(entt::registry& reg, entt::entity actor, Deed kind, std::int32_t mag) {
  reg.get_or_emplace<BehaviorLedger>(actor).dims[static_cast<std::size_t>(kind)] += mag;
}
```

Two details carry the design:

- **Lazy** — `get_or_emplace` means an entity earns a ledger only on its first deed.
  Anyone who never acts (and the whole world before this existed) has no ledger and
  replays **bit-identically**. Morality costs nothing until it happens.
- **Parity for free** — it takes a bare entity with no player/NPC branch, so a player
  and an NPC accrue deeds through the exact same path. A rescuing NPC and a rescuing
  player earn the identical Charity.

`standing` is a **pure function** of the ledger — it reads no sim state, so it is
trivially testable and can never disagree with the deeds it sums:

```cpp
standing = charity·4 + valor·5 + honesty·3 + loyalty·3 − cruelty·6 − violence·4
```

Those are the design's exact weights (`.8 / 1.0 / .6 / .6 / −1.2 / −.8`) scaled **×5**
— the smallest factor that turns every one into an integer, so **no float enters the
sim** and replay stays bit-identical. The unit is "fifths of a design-point"; a single
rescue is `+4` standing.

Four deeds are wired. The first lives in `handle_deaths`: the rescue loop already finds
the ally who reaches a downed player — it now keeps that **entity** (not just a yes/no)
and, after hauling them up, credits it `Deed::Charity`. The rescued player earns
nothing; the *rescuer* does. On that same rescue, if the rescuer's affinity toward the fallen
was **already at or above the bond floor** (`kBondPull`, +10 — a real prior tie, checked
*before* the rescue's own affinity bump), it also earns `Deed::Loyalty`: standing by a friend
is worth more than charity to a stranger, so a bonded save reads as `+7` standing (Charity ×4 +
Loyalty ×3) against a stranger's `+4`. The next lives in `perform_attack`: the blow that takes
a hostile creature's HP across zero credits the **attacker** with `Deed::Valor` — scored
on the alive→dead *transition*, so only the one fatal strike counts, never a second
swing on the already-dead foe.

The fourth also lives in `perform_attack`, on the branch that used to be a plain **whiff**.
When a swing finds *no* hostile in reach, a **player** (only a player) instead strikes the
nearest peaceful colonist for the same STR-vs-VIT damage, and earns `Deed::Cruelty`. Three
gates keep it a **choice, never a slip**: only a player swings this way (NPCs never turn on
the colony — an NPC-villain AI is a later ring), hostiles are always searched *first* and win
the target (so you can reach a colonist only with nothing else to fight), and it must be in
reach. A downed body is excluded — no infamy for kicking a corpse. And if that blow **kills** the
colonist (drops it to 0 HP), the death earns `Deed::Violence` *on top of* the Cruelty — the
escalation from wounding to slaying, sinking standing by the Cruelty **×6** *and* the Violence **×4**
(−10, versus −6 for a non-lethal betrayal). A cruel strike that only wounds stays Cruelty-only.

Only **unjust** violence counts, though: the extra Violence lands only when the victim's own
`standing` was **≥ 0** — an innocent. Kill a colonist who had *themselves* turned bad (a below-zero
standing earned through their own cruelty) and the death is **rough justice**, not villainy: the blow
is still Cruelty (you struck a person who wasn't fighting you), but no Violence on top — the design's
*"killing bandits barely dents you"*. Every colonist starts neutral (standing 0), so today this is
dormant wiring — a victim can only sink below zero once NPC-villain AI or a villainous co-op player
exists — but it keeps the Violence tally faithful the moment a wrongdoer can be on the receiving end.

A cruel strike also lands a *personal* mark: besides the Cruelty deed, it forms a **grudge** — the
struck colonist's affinity toward the striker drops, and a grudge-holder later **refuses to rescue**
that player. That is a finer, earlier consequence than the global *fear* below (which needs standing
past the Suspect line): hurt one colonist and *that one* abandons you long before the whole colony
does. See [relationships](relationships.md).

!!! info "Three heroes and a villain"
    Rescue → Charity, hostile-kill → Valor, and rescuing a *bonded* ally → Loyalty are the
    things a hero *does*; striking a peaceful colonist → Cruelty is the mirror. Each is **one
    `record_deed` call**, exactly what shipping the seam and the full schema first bought — every
    new deed appended a *value*, never a *field*. Cruelty is weighted **×6** (dearer than any
    single hero deed), so one betrayal drops `standing` to −6: villainy is cheap to commit and
    expensive to wear — and a betrayal that **kills** adds Violence (**×4**) on top, −10 in all. Only
    **Honesty** now waits on its event (a truth/deceit act, which the current systems don't yet have).

## Drift: deeds reshape character

Morality and personality aren't two separate ledgers — a deed does *two* things at the
one `record_deed` write-point. Besides adding to the ledger, it **drifts the actor's
matching `Personality` axis** a small, bounded step: **Valor → bravery** (fighting
monsters hardens you), **Charity → compassion** (hauling up the fallen softens you),
**Loyalty → loyalty** (standing by your own deepens the leaning), the villain
mirror **Cruelty → compassion *down*** (striking the helpless hardens you back toward
callous — the one deed that *lowers* its axis), and **Violence → bravery** (the kill steels the
nerve — a killer grows desensitized, not softer; so a *lethal* cruel strike reshapes two axes at
once, cooling compassion via Cruelty **and** warming the nerve via Violence). This
is the design's *"you are what you do"* / "the war changed him" — a character reshaped by
its own history, not just a fixed dial rolled at birth, with hero deeds lifting and villain
deeds lowering.

The step is deliberately small (`kDeedDriftStep`, ~50 deeds for a full ±100 swing), so a
handful of deeds shifts you visibly but never into a wholly different person. And it lands
where it can be *seen*: bravery is the **tinted** axis and the one `steer_npcs` reads
twice (flee radius + rescue commitment), so a colonist that keeps fighting warms toward
yellow and starts holding its ground — a character arc read from its deeds alone.

Two guard-rails carry it. It uses `try_get`, **never** `get_or_emplace`: an actor with no
`Personality` (every creature) accrues the deed on its ledger but *stays* Personality-free, so the
bit-identical absent-Personality world is preserved. The **player** now carries a **neutral**
`Personality` (the design's *"players start neutral, drift from deeds"*), so its own deeds reshape it
too — a Valor kill warms it, a Cruelty strike hardens it. That stays sim-bit-identical because no sim
system **reads** the *player's* Personality: `steer_npcs` (the only reader of a bravery axis) iterates
the `Npc` view, and the player isn't an `Npc`. Its Personality *is* written — here, and by **grief**
once the player has bonded (grief is a `<Personality, Relationships>` view a rescued player joins) —
but those writes are sim-inert, so the drift is a **render-only** character arc: the player's blue dot
warms with its deeds, nothing more. **Five** deeds drift now — every deed that has an event — so only
**Honesty** waits, and only because no honesty/deceit event exists yet.

Drift isn't only earned by *your own* deeds — it can be dealt by **loss**. When a colonist
you were truly bonded to (**Friend** or above, see [relationships](relationships.md)) is
slain, **grief** drifts your bravery *down* a step (`kGriefDrift`, the negative mirror of a
Valor deed's bravery-*up*): fighting monsters hardens you, watching one of your own fall
**shakes** you. It fires in `handle_deaths` the moment the fallen is reaped — a rattled
mourner then flees a hazard sooner (that same bravery→flee radius), so **permadeath leaves a
mark on the living**, not just an empty slot. Both drift sources share one `drift_axis` clamp,
so a deed and a bereavement can never bound differently. Only a real friend-bond grieves — a
mere acquaintance (or a dead rival) is no such loss, the gate that keeps the pre-bond world
bit-identical.

## What to expect

You can now **see** it both ways. A character's dot **scales with its `standing`**: it *swells*
as a colonist keeps rescuing allies and felling monsters (visibly a figure of repute) and
*shrinks* as they strike their own (dwindling to a shunned husk) — symmetric about neutral and
capped at both ends (`renown_scale`), so nobody balloons or vanishes. A presentation-only reader
(the renderer, never the sim, so determinism holds). **Size** is the free channel: colour already
carries bravery, brightness carries health. And the HUD prints the player's `standing` *number*
and `standing_title` to match, so a **fall** now reads on *both* the dot and the label — strike
your own colonists and the number goes negative, the dot draws small, and the title flips *Known →
Suspect → Notorious*.

And now standing **acts**, not just shows — *both* ways. A player who crosses the *Suspect* line
becomes a **threat colonists flee**; a player — **or a renowned NPC colonist** — who crosses the
mirror *Known* line becomes a **hero they rally to** (heroism is earnable by anyone, since Valor and
Charity aren't player-gated the way Cruelty is, so the rally rung rallies to a famous NPC too — see
[NPC behaviour](npc-behaviour.md)). `steer_npcs` folds a villain into its top-priority danger rung
(turn on the colony and it recoils from you like a hazard) and a hero into its bottom-priority rally
rung (idle colonists gather around their champion). Villainy repels, heroism attracts — the two faces of one
scalar (see [NPC behaviour](npc-behaviour.md)). And a fallen villain earns no mercy: the rescue rung
reads standing too — **both ways**. On the dark side, **the colony abandons a downed villain**:
nobody crosses the field to save one whose deeds marked it, nor lifts it at point-blank (both the
steer rescue-seek *and* the `handle_deaths` revive check it, in lockstep) — the **global** counterpart
of the personal [grudge-veto](relationships.md) (a grudge is one colonist you wronged; this is the
*whole* colony turning its back on infamy). On the bright side, **the colony rushes to a downed
hero**: fame *discounts* the distance the rescue rung will cross, so a fallen champion is reached from
*farther* than a neutral, even by a stranger (`kHeroReachDiscount`) — the public-fame twin of the
personal [affinity-graded reach](relationships.md), the two stacking so a bonded hero is worth the
longest trek of all. So standing shapes not just who the colony *fears* but who it *saves*. The
*richer* readers — graded wariness, protecting
the weak, befriending — are a later ring, but the "believed standing changes behaviour" seam is now
real on both sides. Under the hood it is pinned by tests: a rescuer's ledger gains Charity, a
monster-slayer's gains Valor, a player who cuts down a colonist gains Cruelty and goes negative
(and colonists then flee that player, but not a hero or an unproven one); a bystander, an unrescued
timer-expiry respawn, a chip that doesn't kill, and a colonist shielded by a nearby hostile all
record nothing.

## The tradeoffs

- **`standing`'s gameplay readers are still coarse.** Colonists **flee** a villain and **rally** to a
  hero (`steer_npcs`), and the downed-rescue reads standing **both ways** — **abandoning** a downed
  villain (no rescue, in `steer_npcs` *and* `handle_deaths`) and **rushing** to a downed hero (reached
  from farther). All are *binary* at a single threshold — the
  design's graded *perceive* (the pull/push scaling with standing *and* the onlooker's own
  bravery/might, plus richer stances: befriend, protect the weak, exploit) is a later ring. The
  signed formula shipped in full before any of this was needed, so wiring Cruelty, the fear read,
  and the rally read were each one-line-ish additions — the payoff of locking the schema early.
- **Cruelty is player-only.** An NPC can't yet turn on the colony — `perform_attack` gates
  the cruel branch on `PlayerControlled`, because it is shared with `npc_attack` and a
  generic "hit a neighbour when no enemy is near" would make colonists brawl constantly. The
  NPC-villain path (a personality/standing-driven decision to harm) is a social-ring job.
- **Magnitudes are hardcoded.** `kRescueCharity`, `kValorKill`, `kCrueltyStrike`, `kViolenceKill`
  are constants; JSON-authored deed weights are a modding-milestone job. A tuning knob, not a
  design gap.
- **`int32`, not fixed-point.** The ×5 integer trick sidesteps the codebase-wide
  fixed-point migration (a later ring) while staying bit-identical.

## Where it goes next

The first three **titles** here are all pure queries — the design's "titles are derived queries,
never stored slots" — and they read three *orthogonal* things about you. `standing_title(standing)`
names your *repute* from **deeds** — *Unproven* → *Known* → *Renowned*, and its villain mirror
*Suspect* → *Notorious* that Cruelty deeds now reach (strike your own and the title falls). Its twin
`build_title(attributes)` names your *build* from what you've **trained** — a *Warrior* (STR),
*Skirmisher* (DEX), *Bulwark* (VIT) or *Chancer* (LCK), *Greenhorn* until one leads. The third,
`deed_epithet(ledger)`, names what you're **known for** — your single *most-repeated* deed once you've
done it enough (`kEpithetAt`) to earn the name: *the Slayer* (Valor), *the Savior* (Charity), *the
Faithful* (Loyalty), *the Butcher* (Cruelty), *the Brutal* (Violence, now reachable since a lethal
cruelty feeds it), with only *the Honest* (Honesty) band still awaiting its deed. It's the first
reader of a **single** ledger dimension — the six were only ever
summed into `standing` before — so how good you are, what you fight as, and what you're famous *for*
are three separate labels. Unlike the always-present band titles it returns nothing until a kind
crosses the threshold, so the HUD only shows *known as* once you've truly earned a reputation. The
richer ones (*Master Smith*, *Dragonslayer* — from specific skills and gear) hang off the same idea.

A further recognition **conjoins** two of the others into the design's named **role**: `hero_role(ledger)`
reads a colonist a **Champion** or a **Fiend**, but only when fame *and* deeds AGREE. A Champion is
*Renowned* (standing ≥ `kRenownFullAt`) **and** dominantly a *Slayer* (Valor the top deed); a Fiend is
its mirror — *Notorious* **and** dominantly a *Butcher* (Cruelty). Where `standing_title` shows the
fame alone and `deed_epithet` the deed alone, this is the one label that demands **both**: not just a
reputation but that it was earned the heroic (or villainous) way. So a Renowned *Savior* — famous, and
for a good deed — is celebrated but **no Champion**; the role is a martial pole, `HERO`/`VILLAIN`
distinct from the epithet's nuance, the emergent identity the world's future reactions can key on. A
pure query (same dominant-deed argmax as `deed_epithet`); `nullptr` for everyone who isn't at a pole,
so the HUD shows *role* only for the truly exceptional.

A further title reads neither deed nor build but **who you ARE**: `temperament_title(bravery)` names how
brave or cowardly you've become — *Steady* by default, drifting to *Bold* / *Fearless* or *Timid* /
*the Coward* (the design's named badge) as Valor and Violence deeds and a friend's death (grief)
reshape your **bravery**. It's the panel-text twin of the `personality_tint` that already warms or
cools your dot by that same axis, so your temperament now reads in *words* as well as colour — the
first title off the **personality** axes, orthogonal to the deed/build/repute trio, and it makes the
design's "the war changed him" legible: an always-present band that shifts as the character is shaped.

One more reads **how BROADLY** you've grown rather than how high: `versatile_title(attributes)` is
the generalist counterpart to `build_title`'s peak. Where `build_title` names your single dominant
*combat* attribute, this counts how many of **all seven** are meaningfully trained (past
`kVersatileAt`) — including the non-combat **Wisdom / Charisma / Intellect** that `build_title`
deliberately ignores (the *Naturalist*-style title its comment always promised) — and names the
*shape*: **the Versatile** (3–4 developed sides) → **the Polymath** (5+). Like the epithet, and
unlike the always-present bands, a specialist earns **nothing** (`nullptr`) — `build_title` already
names their one peak — so the HUD shows *breadth* only once you've genuinely spread yourself wide. A
fresh character has trained nothing past level 1, so the badge is absent and bit-identical until earned.

The write-point is the whole point: **Violence** just became one more `record_deed` call at the
lethal-cruelty site (exactly as Cruelty and Loyalty landed), leaving only **Honesty** to wire the day
a truth/deceit event exists. `standing`'s
first **gameplay** reader has already landed — colonists flee a villain — and it grows from there
into the full social `perceive` layer that turns a character's *believed* standing into how others
treat them (befriend, protect, exploit, not just fear), graded by the onlooker's own nerve rather
than a single threshold.

## Leaky standing — reputation fades

Deeds don't mark you forever: **`decay_standing`** leaks every ledger dimension one step toward `0`
every `kDecayPeriod` ticks, so a reputation **fades if it isn't renewed** — the design's *redemption
and corruption for free*. A villain who stops being cruel climbs back toward neutral; a hero who
rests on old glory dims. It's symmetric about zero (both signs creep in at the same rate) and runs
on an **exact integer tick-count** per ledger (`BehaviorLedger::decay_ticks`, no float, so it stays
bit-exact), touching only actors who've actually done a deed. `kDecayPeriod` is a fast playtest knob
for now — the design's *"~44-day leak"* is far slower — but the *shape* (a slow current pulling every
reputation back to neutral, so standing must be *maintained*, not banked once) is the point. Because
nothing reads `decay_ticks` and no whole period elapses in a short run, a brief world is
bit-identical to before decay existed.

## Key files

- `engine/sim/components.hpp` — `Deed` (the six dimensions), `BehaviorLedger` (the
  earned counterpart of `Personality`, plus its `decay_ticks` leak counter), the pure `standing`
  function, `renown_scale` (the presentation twin of `personality_tint`), and the derived recognition
  titles `standing_title` (repute from deeds), `build_title` (from trained attributes), `deed_epithet`
  (what you're known for — the dominant single ledger dimension, past `kEpithetAt`), `veteran_title`
  (how seasoned, from character level), `temperament_title` (how brave, from the personality
  bravery axis — the panel-text twin of `personality_tint`), `versatile_title` (how BROADLY
  trained — the generalist counterpart to `build_title`'s peak, reading all seven attributes), and
  `hero_role` (the design's Champion/Fiend ROLE — the conjunction of Renown *and* a dominant
  Valor/Cruelty deed, where the other titles each read only one axis).
- `engine/sim/systems.hpp` / `systems.cpp` — `record_deed` (the single write-point,
  which also **drifts** the actor's matching `Personality` axis via the shared `drift_axis` clamp);
  `decay_standing` (the slow leak toward neutral, run each `step()`); the Charity credit and the
  bonded-ally Loyalty credit (`kBondPull`-gated) in `handle_deaths`' rescue branch, the Valor
  credit in `perform_attack`'s killing-blow branch, and the Cruelty credit — plus the **Violence**
  credit when that cruel blow *kills* (`kViolenceKill`) — in the same
  function's player-only "no hostile in reach" branch (`kCrueltyStrike`). `handle_deaths` also
  drifts a **second** way — **grief** (`kGriefDrift`, the same `drift_axis`): a survivor bonded to
  a just-reaped colonist (`kBondFriendAt`-gated) loses a step of bravery.
- `game/app/main.cpp` — `draw_entities` scales a dot's radius by `renown_scale(standing(...))`
  so standing reads on screen *both ways* (heroes swell, villains shrink), and the debug HUD
  shows the player's `standing` number and its titles (`veteran_title` as *rank*, `standing_title`,
  `build_title`, its `breadth` twin `versatile_title` — shown only once you've trained 3+ attributes
  wide — `temperament_title`, the `known as` epithet (shown only once a deed kind crosses
  `kEpithetAt`), and the `role` line (`hero_role` — a gold *Champion* / red *Fiend*, shown only at a
  fame-and-deed pole).
- `tests/sim/test_simulation.cpp` — the funnel + signed formula, the wired deeds with
  player==NPC parity, the lazy no-deed-no-ledger path, `renown_scale`, `deed_epithet` (threshold,
  dominant-dimension pick, and the fixed tie order), and `versatile_title` (both count-band edges,
  the specialist-earns-nothing case, and breadth built purely from the non-combat attributes).

## Go deeper

- [NPC behaviour](npc-behaviour.md) — `Personality` (who a character *is*), the innate
  twin of this earned ledger; the rescue rung that morality now records.
- [Combat](combat.md) — the Downed death beat that a rescue interrupts.
