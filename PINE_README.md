


savestate/loadstate algorithm explanation:

Savestate:
We track which pages of ram were written to during each frame. We record those dirtied pages into a ring buffer of "slots".
So at any given time, one of the ring buffer slots is the "current frame" and as you move backward in the ring buffer, we go further back in time.
At the beginning of each frame, we take the pending dirty pages (ones written to during the previous frame)
and save only those dirty pages into the current ringbuffer slot.
For reasons i'll explain, we also have a "base" snapshot of all of game ram - we initialize it on the first frame of the game.
Each time we do a savestate, we "evict" the oldest savestate "delta" from the ring buffer. We "apply" that delta onto the base snapshot.
So that base snapshot will always contain a full snapshot of game ram NUM_SAVE_SLOTS frames ago.

Loadstate:
- target frame = the frame we want to rollback to
- CURRENT = the current frame we are at... most recent

At a high level, you can imagine rolling back into the past as taking the current game memory, and "applying" the deltas
we've been saving (in the ring buffer) in reverse chronological order until we get to the frame we needed to rollback to.
"applying" a delta means copying the saved dirty pages from that slot in the ring buffer into the current/active game memory.

This is where loadstate gets a lil more complex - for the sake of both optimization and correctness.

(optimization)
If a page is dirtied in multiple deltas, we only need to apply the most recent one (closest to current) since that will have the most up-to-date data for that page.

(correctness problem)
Each delta slot records the post-frame state of dirty pages — not the pre-frame state.
So we can't simply "undo" by applying deltas in reverse; we have no record of what a page
contained before a given frame wrote it.
I.E. if we are at frame 5, and want to rollback to frame 4, and a page was written to on frame 4, we actually want to apply frame 3's delta
So, for each page dirtied in (target, current], we find the most recent slot AT OR BEFORE
target that captured it — that slot's copy is the correct value for the target frame.
Pages with no delta anywhere in the ring come from the base snapshot.

To find the correct source for each page, we do two passes over the ring buffer:

1) Forward pass [target -> current] : mark every dirtied page in a flat byte array (one byte per page, fits in L1).
2) Backward pass [target -> oldest] (each iteration is backward in time): for each slot, satisfy any still-marked pages
   found there (recording that slot as the source), then clear the mark. Stop when all pages are
   satisfied. Remaining marked pages have no delta in the ring — use the base snapshot.

```
  base       |<- pre-window ->|<------------- rollback window ------------->|
  snapshot   [slot] ...[slot]  [TARGET] [slot] [slot] ... [slot] [CURRENT]
                  |                |                                    |
                  |                +------ forward: mark/collect all dirty pages ---+-->
                  |                |
                  +<--- backward: satisfy marked pages --------+
                  |
           first hit = source   (no hit = use base snapshot)
```

Could continue parallelizing loadstate, still some easy wins to be had there (though at this point it's kinda diminishing returns, loadstate is no longer much of a bottleneck)


## Game sim

would love to have a integration into a profiler for every JIT block, and maybe can symbolicate offline somehow (chrome tracing format?)
Tried implementing a JIT Tracer. Current bookmark is symbols aren't symbolicating and the chrome tracing events were formatted a little weirdly.

## Netplay
- desync detector
  - include some normal game state like player pos, stocks, percent, speed maybe?
  - also optionally (debug/dev only) include a hash of savestates or ram or something. Some way to say "hey we desynced, and the savestate hashes don't match so it's a savestate problem rather than... something else?"

BOOKMARK: diagnose the desyncs.

## Buuugs
- end of match freeze
- Some randomly super-expensive GameSimFrame zones... some are filled with real work it seems, some are filled with CoreTiming::Throttle -> Sleeps!
    - for the sleeps ones, maybe it's rollback related? We can probably fiddle with the throttle logic for rollbacks/resims which might help?
- Some randomly super-expensive BrawlbackFrame calls that DONT have GameSimFrame inside. Untracked by tracy, idk what's happening there.
^ NEED TO DIAGNOSE/FIX THESE BEFORE SHIP! From local testing, these really hurt the experience.

## Misc
Superluminal doesn't basically doesn't work at all with dolphin. Since most of the code is through JIT, it can't resolve symbols b/c it relies on (symbolicated) stack traces
I think there's a way to fix this - windows seems to support this sorta thing.
https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlinstallfunctiontablecallback
https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtladdfunctiontable

[COMMON] prints "Want determinism -> false" when booting. We'll want to turn this on for ship.

## Cleanup
all the assorted atomics in rollbackmanager can be merged into 1 that gets checked everywhere
There's all the stuff in the brawlback folder, and the stuff in the core/Rollback folder. These should get put together.

remove all the old rollback infra.
there's a bunch of code files in different folders, bring them all into one place.