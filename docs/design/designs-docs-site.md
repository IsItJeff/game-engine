# Documentation Website Design — MkDocs Material

## 1. Information Architecture

**One site, two top-level sections** (Material `navigation.tabs` gives you a tab bar: *Handbook* | *Engine Docs* | *Roadmap*). Not two sites — shared search, shared style, shared cross-links are the whole point.

### Landing page (`index.md`)
Does exactly three things, above the fold:
1. **Two doors**: "I want to learn engine dev" → Handbook; "I want to use/mod the engine" → Engine Docs. Two big cards, one sentence each.
2. **One copy-paste quick start** (clone + build + spinning cube) — proof of life before anyone reads anything.
3. **Honest status banner**: "Pre-1.0. APIs will break. Here's the roadmap." Builds trust; costs nothing.

No feature-marketing wall. That comes when there are features to market.

### Handbook nav tree (learning content, track-based)

```
Handbook/
  Start Here/            # how to use this handbook, learning paths, "pick your track"
  C++ for Game Devs/     # coming-from-another-language track: RAII, ownership,
                         #   headers/linking, CMake, value vs reference semantics,
                         #   debugging, common footguns
  Engine Architecture/   # game loop, ECS, layers, data-driven design, asset pipeline
  Rendering/             # GPU mental model, SDL3 GPU API, shaders, lighting, PBR intro
  Physics/               # integration, collision, character controllers, Jolt concepts
  Netcode/               # client-server, prediction, reconciliation, interpolation,
                         #   listen vs dedicated servers, NAT/Steam relay
  Animation/             # skeletal, blending, state machines, glTF as source format
  AI & Navigation/       # navmesh, steering, behavior trees, Recast/Detour concepts
  Scripting & Modding/   # embedding Lua, sandboxing, API design for modders
  Tooling/               # ImGui editors, hot reload, profiling, serialization
  Shipping/              # Steam, builds/CI, crash reporting, save data, patching
```

Each track folder gets an `index.md` overview page (Material `navigation.indexes`) with a Mermaid map of the pages in reading order.

### Engine Docs nav tree

```
Engine Docs/
  Getting Started/       # install, first project, first mod — 3 pages max
  Guides/                # task-oriented: "add an NPC", "host a co-op session",
                         #   "make a genre preset", one task per page
  Modding/
    Concepts/            # module model, sandbox rules, capabilities/permissions
    API Reference/       # generated or hand-curated per-system reference
    Examples/            # complete runnable example modules
  Multiplayer/           # single-player, listen server, dedicated server setup
  Companion Services/    # second-screen inventory/map API
  Architecture/          # ADRs: one decision per page (numbered, immutable)
  Roadmap & Changelog/
```

### Cross-linking (the differentiator)
Fixed reciprocal footers, enforced by the page template:
- Every Engine Docs page ends with **"Theory: why it works this way"** → handbook page(s).
- Every Handbook page ends with **"In this engine"** → the engine docs page implementing the concept.
- Shared tag taxonomy via Material's **built-in `tags` plugin** (`rendering`, `netcode`, `modding`, …) so search and tag pages cut across both sections.

## 2. ADHD/Dyslexia-Friendly Style Guide (enforceable rules)

All features below verified as natively supported by Material unless marked *(plugin)*.

**Hard limits (checkable in CI with a 20-line script):**
- **≤ 900 words of prose per page** (~4 min read). Longer → split.
- **One concept per page.** Test: the title has no "and". "Prediction and Reconciliation" = two pages.
- **≤ 4 consecutive paragraphs** without a diagram, code block, table, or admonition breaking them.
- **≤ 3 heading levels** (H1 + H2 + H3, no deeper).

**Fixed page template** — every page uses these H2s in this order; omit a section if empty, never reorder:
1. **What it is** (2–3 sentences, no jargon before its definition)
2. **Why you care** (1 short paragraph tying to a concrete game situation)
3. **Quick start** (copy-paste-runnable — see code rule below)
4. **How it works** (diagrams-first)
5. **Pros / Cons** (table, always a table)
6. **What to expect** (honest: "this takes a weekend", "everyone's first version has bug X")
7. **Go deeper** (sources + cross-links + one curated video with duration)

**Reading time:** Material's built-in readtime only covers its blog plugin, not regular pages. Use **`mkdocs-timetoread-plugin`** *(third-party, lightweight, verified on PyPI)* to inject "~4 min read" after each H1. Fallback if it rots: the 900-word cap makes every page "under 5 min" anyway.

**Admonition rules** (Material native, incl. collapsible via `pymdownx.details`): max 3 per page. Fixed semantics only — `tip` = shortcut, `warning` = footgun that costs hours, `info` = "what to expect" reality check, collapsible `note` = optional depth. Never use admonitions for core content (screen readers and skimmers skip them).

**Diagrams over prose:** any sequence, state machine, or architecture described in >2 sentences must be a **Mermaid diagram** — Material renders `mermaid` fenced blocks natively via `pymdownx.superfences` custom fences, theme-aware in light/dark, zero JS setup (verified). One diagram per "How it works" minimum.

**Code blocks:** every block must be runnable as pasted — full includes, full `main()` where relevant, or explicitly labeled `// fragment — see full example: <link>`. Enable native `content.code.copy` (copy button) and **`content.code.annotate`** (numbered inline annotations — explains code without a prose wall; ideal for ADHD readers). Use content tabs (`pymdownx.tabbed`) for Windows/macOS variants so the page shows one path at a time.

**Dyslexia typography:**
- Font: set `theme.font` to **Atkinson Hyperlegible Next** (or **Lexend**) — both on Google Fonts, both designed for legibility, so Material supports them natively with one config line. OpenDyslexic is *not* on Google Fonts; skip self-hosting it unless a user asks (it also tests worse than Atkinson in studies).
- `extra.css`: `--md-typeset` line-height 1.7; max line length ~70ch (Material's content column is already close); left-align only, never justify.
- No italics for emphasis (hard to read dyslexic) — use **bold**.
- Skip building a font-switcher widget: custom JS, no native support. Add only if users request it.

**Progressive disclosure:** deep-dive material goes in collapsible `??? note "Under the hood"` blocks or a linked page — never inline. Tabs for platform/language variants.

**Consistent visual anchors:** same emoji/icon per track (Material has native icon/emoji support), same H2 skeleton everywhere, tags chip row at top. Predictability is the accessibility feature.

## 3. Research → Verify → Publish Pipeline

Honest framing up front, on the site itself: *"Every page is fact-checked against primary sources before publishing. Process reduces errors; it cannot guarantee zero. Found one? File an issue — there's a button on every page."* (`edit_uri` gives a per-page edit/issue link.)

**Per-page pipeline (states tracked as PR labels, not extra tooling):**
1. **Research** — gather 2+ independent sources per factual claim; primary sources (spec, official docs, library source code) outrank tutorials.
2. **Draft** — write to the template; every non-obvious claim carries an inline source anchor.
3. **Adversarial verify** — a second pass by a *different* agent/session with the explicit prompt: "Find errors. Check every claim against its cited source. Compile and run every code block. Check every version number against today's releases." Reviewer produces a findings list; zero unresolved findings to proceed. This is the step that makes "verified" mean something.
4. **Publish** — PR merges only with the DoD checklist ticked.

**Citation style:** a "Sources" block at page bottom (inside "Go deeper"): title, author, URL, and **accessed date** — the accessed date is what makes rot detectable. No academic citation ceremony.

**Rot policy:**
- CI on every PR: `mkdocs build --strict` (catches broken internal links/anchors natively).
- Weekly scheduled **lychee** run over the built site for external links; failures auto-open a GitHub issue.
- Pages carry last-updated via `mkdocs-git-revision-date-localized-plugin` (verified, actively maintained). Any page untouched for 12 months gets a review issue — versions and library claims age fastest.

**Definition of done (checklist in the PR template):**
- [ ] Follows the fixed template; ≤900 words; ≤3 admonitions
- [ ] Every code block compiled/ran as pasted, on the version stated
- [ ] Every factual claim traceable to a source in the Sources block
- [ ] Adversarial review passed with zero open findings
- [ ] ≥1 Mermaid diagram if the page explains a mechanism
- [ ] Cross-links both directions (handbook ↔ engine docs)
- [ ] Tags assigned; nav entry added; `--strict` build green

## 4. Source Catalog (all verified live, July 2026)

### Text — by track

| Source | Track | Uniquely good for |
|---|---|---|
| [learncpp.com](https://www.learncpp.com) | C++ | The canonical free modern-C++ course; actively maintained; best sequencing for someone new to C++ specifically |
| [cppreference.com](https://en.cppreference.com) | C++ | Reference, not tutorial; the "Sources" primary source for any C++ language/stdlib claim |
| [learnopengl.com](https://learnopengl.com) | Rendering | Best-structured graphics fundamentals on the web; concepts (lighting, PBR, shadow maps) transfer to SDL3 GPU even though API examples are OpenGL |
| [gafferongames.com](https://gafferongames.com) | Netcode/Physics | Glenn Fiedler's canonical series: fixed timestep, UDP, snapshot compression, networked physics. Site live; note he's since moved new writing to mas-bandwidth.com |
| [Gambetta — Fast-Paced Multiplayer](https://gabrielgambetta.com/client-server-game-architecture.html) | Netcode | THE gentle intro to prediction/reconciliation/interpolation, with live interactive demos; assign before Gaffer |
| [gameprogrammingpatterns.com](https://gameprogrammingpatterns.com) | Architecture | Nystrom's full book free online; game loop, component, data locality chapters map directly onto engine design |
| [redblobgames.com](https://www.redblobgames.com) | AI/Nav | Interactive explanations of A*, hex grids, procgen; the gold standard your diagram policy is imitating |
| [Jolt Physics docs](https://jrouwe.github.io/JoltPhysics/) | Physics | Official architecture guide (bodies, shapes, constraints, character controllers); primary source for physics-integration pages |
| [SDL3 wiki](https://wiki.libsdl.org/SDL3/FrontPage) | Rendering/Platform | Official API reference + SDL2 migration guide; GPU API reference lives under the API section |
| [sol2 docs](https://sol2.readthedocs.io/en/latest/) | Scripting | Official C++↔Lua binding docs — tutorials, usertypes, performance; primary source for the modding-runtime pages |
| [recastnav.com](https://recastnav.com) | AI/Nav | Official Recast/Detour docs: navmesh generation pipeline, DetourCrowd; pairs with Red Blob's theory |
| [glTF 2.0 spec + samples](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html) | Animation/Assets | Normative spec plus Khronos sample-assets repo; primary source for asset-pipeline claims |
| [Dear ImGui](https://github.com/ocornut/imgui) | Tooling | Repo README + demo code *is* the documentation; canonical for editor/debug UI |
| [MkDocs Material docs](https://squidfunk.github.io/mkdocs-material/) | (meta) | Reference for your own site; also the best living example of these accessibility patterns |

### Video — verified and characterized

| Channel | Status | Curate for |
|---|---|---|
| **TheCherno** (Game Engine / Hazel series) | Active | The only long-form "build a C++ engine as a product" series; matches this project's shape almost exactly. Curate the engine-series playlist, not the whole channel |
| **Handmade Hero** (Casey Muratori) | **On hiatus** — full archive + searchable [episode guide](https://guide.handmadehero.org) remain up | From-scratch, no-libraries philosophy; curate specific episodes (memory arenas, timing, multithreading) via the guide's timestamps, don't assign "watch the series" (600+ eps) |
| **Sebastian Lague** | Active | Coding Adventures — the best "why this is exciting" motivational material; assign as track openers, not as instruction |
| **Acerola** | Active | Shader/post-processing deep dives with strong visual intuition; rendering track |
| **Freya Holmér** | Active | Math for game devs — splines, vectors, quaternions; the definitive visual math explainers |
| **SimonDev** | Active | GPU/3D fundamentals from an industry veteran; more rigorous than most, good bridge from Lague to LearnOpenGL |
| **jdh** | Active | Entertaining from-scratch engine/game projects; motivation-tier, links things like software rasterizers |
| **Vercidium** | Active | Short, concrete optimization deep dives (voxel/renderer perf) |
| **GDC on YouTube / GDC Vault** | Active | Curate individual talks per track (e.g. Overwatch netcode ECS talk for netcode; *It's All in the Head* for animation). Vault talks: link only |

**Embedding etiquette:** **link, don't embed.** Embedding YouTube iframes is permitted by YouTube ToS when the creator enables it, but embeds add page weight, trackers, and an attention trap — the opposite of your accessibility goals. Every video link states duration and what to expect: *"Video: Prediction explained visually — 22 min — watch after reading this page."* If you ever embed, use `youtube-nocookie.com`. Never download/re-host. GDC Vault content: always link.

## 5. Repo & Deployment

**Verdict: `docs/` inside the engine repo, deployed on Cloudflare Pages.**

- **Same repo** because docs PRs must ride alongside code PRs (your DoD requires it), and one repo halves solo-dev overhead. The private-repo concern is solved at the *host* level, not by splitting repos.
- **Cloudflare Pages** because (verified): its **free tier builds from private repos** — GitHub Pages requires a paid plan for private-repo Pages; free plan is public-repos-only. CF free tier: 500 builds/month, effectively unlimited bandwidth, 20k files/site — ample. So the engine repo can go private later without touching docs hosting. Netlify works too but its 300 free build-minutes/month is a tighter ceiling; no advantage here.
- Split into a separate docs repo only if the engine goes private **and** you want external doc contributors without engine access. Don't pre-build for that.

**mkdocs.yml skeleton** (everything here verified supported):

```yaml
site_name: <Engine> Docs
site_url: https://docs.example.com
repo_url: https://github.com/IsItJeff/game-engine
edit_uri: edit/main/docs/
theme:
  name: material
  font: { text: Atkinson Hyperlegible Next, code: JetBrains Mono }
  features:
    - navigation.tabs        # Handbook | Engine Docs | Roadmap
    - navigation.indexes     # section overview pages
    - navigation.footer      # prev/next — linear paths for the handbook
    - toc.follow
    - search.suggest
    - search.highlight
    - content.code.copy      # copy button on every block
    - content.code.annotate  # inline code annotations
  palette:                   # light + dark, auto
    - media: "(prefers-color-scheme: light)"
      scheme: default
    - media: "(prefers-color-scheme: dark)"
      scheme: slate
markdown_extensions:
  - admonition
  - pymdownx.details         # collapsible blocks
  - pymdownx.superfences:
      custom_fences:
        - { name: mermaid, class: mermaid,
            format: !!python/name:pymdownx.superfences.fence_code_format }
  - pymdownx.tabbed: { alternate_style: true }
  - pymdownx.highlight
  - attr_list
  - md_in_html
plugins:
  - search
  - tags
  - git-revision-date-localized: { type: timeago, fallback_to_build_date: true }
  - timetoread                # reading-time badge (third-party)
extra_css: [stylesheets/accessibility.css]   # line-height 1.7, 70ch measure
```

**Versioning:** **skip `mike` for now.** mike is alive, well-maintained, and Material integrates its version selector natively — but versioned docs only earn their complexity once you make a compatibility promise, i.e. the first tagged release where modders' modules could break on upgrade. Until then it's pure overhead and stale-page risk. The handbook is *never* versioned. When the day comes, mike is a bolt-on, not a migration.

**CI (GitHub Actions):**
- On PR touching `docs/**` or `mkdocs.yml`: `pip install -r docs/requirements.txt && mkdocs build --strict` (fails on broken internal links/anchors) + the 20-line style-limit script (word count, heading depth, template H2s present).
- Weekly cron: build, run **lychee** against external links, auto-file an issue on failures.
- On merge to `main`: Cloudflare Pages builds automatically from the connected repo — no deploy step to maintain.

---

**Key trade-offs accepted:** no font-switcher widget (custom JS, add on request); no `mike` until first stable release; no embedded videos (deliberate accessibility choice); reading-time via a small third-party plugin with a graceful-degradation story; "100% accuracy" reframed honestly as a verification *process* with a visible error-reporting path.

Sources: [Material diagrams (native Mermaid)](https://squidfunk.github.io/mkdocs-material/reference/diagrams/) · [Material admonitions](https://squidfunk.github.io/mkdocs-material/reference/admonitions/) · [Material Python-Markdown extensions](https://squidfunk.github.io/mkdocs-material/setup/extensions/python-markdown-extensions/) · [Material versioning / mike](https://squidfunk.github.io/mkdocs-material/setup/setting-up-versioning/) · [mike](https://github.com/jimporter/mike) · [git-revision-date-localized](https://github.com/timvink/mkdocs-git-revision-date-localized-plugin) · [mkdocs-timetoread-plugin](https://pypi.org/project/mkdocs-timetoread-plugin/) · [readtime is blog-plugin-only](https://github.com/squidfunk/mkdocs-material/discussions/4999) · [Cloudflare Pages limits](https://developers.cloudflare.com/pages/platform/limits/) · [GitHub Pages private-repo limitation](https://github.com/orgs/community/discussions/167331) · [Handmade Hero episode guide](https://guide.handmadehero.org/) · plus the per-source URLs verified in the catalog table above.