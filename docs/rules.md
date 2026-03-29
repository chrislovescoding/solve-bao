# Bao la Kujifunza -- Formal Rule Specification

This document provides the definitive, unambiguous rules for Bao la Kujifunza
(the beginner/simplified variant of Zanzibar Bao). Every ambiguity has been
resolved by cross-referencing the following authoritative sources:

**Primary sources:**
- **[S1]** Donkers, H.H.L.M. (2003). *Nosce Hostem: Searching with Opponent
  Models*. PhD thesis, Universiteit Maastricht. Appendix C: "Zanzibar Bao
  Rules for the Computer" (pp. 163-168), based on de Voogt (1995).
- **[S2]** De Voogt, A.J. (1995). *Limits of the Mind: Towards a
  Characterisation of Bao Mastership*. PhD thesis, Leiden University. The
  original ethnographic rule transcription from Zanzibari masters.
- **[S3]** Kronenburg, T., Donkers, J. & de Voogt, A.J. (2006). "Never-Ending
  Moves in Bao." *ICGA Journal* 29(2): 74-78.
- **[S4]** Donkers, H.H.L.M. & Uiterwijk, J.W.H.M. (2002). "Programming Bao."
  *Seventh Computer Olympiad: Computer-Games Workshop Proceedings*.

**Secondary sources (cross-referenced):**
- **[S5]** Gambiter.com Bao rules page (synthesizing de Voogt/Donkers rules).
- **[S6]** Wikipedia "Bao (game)" article.
- **[S7]** Mancala World wiki "Bao la Kujifunza" page (citing Nino Vessella).
- **[S8]** Polovynka, D. (2026). "Learn how to play Bao la Kiswahili
  step-by-step." Medium article.
- **[S9]** Board Game Arena Bao la Kiswahili implementation.
- **[S10]** Abstract Strategy Games blog: "Bao: sum-up of rules" (2011).

**Relationship between Bao la Kujifunza and full Bao (Bao la Kiswahili):**

Per [S7] (Nino Vessella): "Bao la kujifunza is played with the rules of the
second stage (mtaji stage) of Bao only, but without the takasia rule." This
means: no namua stage, no seeds in hand, no nyumba special rules, no takasia
blocking rule. The game begins directly with all 64 seeds on the board, and
the mtaji-stage rules of [S1] Appendix C apply, minus the nyumba and takasia
complications.


---

## 1. Board Topology

The board has 4 rows of 8 pits (32 pits total). Each player owns two adjacent
rows. [S1 p.163-164]

```
          Left                                    Right
          col 1  col 2  col 3  col 4  col 5  col 6  col 7  col 8

Row b     [P2o1] [P2o2] [P2o3] [P2o4] [P2o5] [P2o6] [P2o7] [P2o8]   <- P2 outer (back)
Row a     [P2i1] [P2i2] [P2i3] [P2i4] [P2i5] [P2i6] [P2i7] [P2i8]   <- P2 inner (front)
Row A     [P1i1] [P1i2] [P1i3] [P1i4] [P1i5] [P1i6] [P1i7] [P1i8]   <- P1 inner (front)
Row B     [P1o1] [P1o2] [P1o3] [P1o4] [P1o5] [P1o6] [P1o7] [P1o8]   <- P1 outer (back)
```

NOTE: Donkers [S1] numbers pits 1-8 from the left of each player. This
document uses 1-based indexing to match that convention.

**Named pits (per player):** [S1 p.164]
- **Kichwa (head):** The two end pits of the inner row -- columns 1 and 8.
  Left kichwa = col 1 (the "clockwise kichwa").
  Right kichwa = col 8 (the "anti-clockwise kichwa").
- **Kimbi:** The kichwa pits PLUS the pits adjacent to them. So the kimbi pits
  are columns 1, 2, 7, and 8 of the inner row.
- **Nyumba (house):** Column 5 of the inner row. In Bao la Kujifunza, the
  nyumba has **no special rules** -- it functions identically to any other pit.

**Opposite pits:** P1 inner column c is directly opposite P2 inner column c.
These face each other across the board.


## 2. Initial Position

Every pit contains exactly 2 seeds. Total seeds = 64. Seeds are never removed
from the board, only redistributed. [S7, S8]

```
  2  2  2  2  2  2  2  2    <- P2 outer
  2  2  2  2  2  2  2  2    <- P2 inner
  2  2  2  2  2  2  2  2    <- P1 inner
  2  2  2  2  2  2  2  2    <- P1 outer
```

Player 1 (South) moves first.


## 3. Sowing Path

Each player's 16 pits form a **closed loop**. Sowing always stays within the
current player's own 16 pits -- seeds never enter the opponent's pits during
sowing. [S1 rule 1.3]

The loop for Player 1 (viewed from P1's side):

```
Clockwise (rightward along inner row):
  P1i1 -> P1i2 -> P1i3 -> ... -> P1i8 -> P1o8 -> P1o7 -> ... -> P1o1 -> P1i1

Anti-clockwise (leftward along inner row):
  P1i1 -> P1o1 -> P1o2 -> ... -> P1o8 -> P1i8 -> P1i7 -> ... -> P1i2 -> P1i1
```

**Key rule from [S1 rule 1.4a]:** If sowing starts at the LEFT kichwa (col 1),
the direction is CLOCKWISE. If sowing starts at the RIGHT kichwa (col 8), the
direction is ANTI-CLOCKWISE.


## 4. Move Structure

**A "move" in Bao is a sequence of sowings and captures by one player.** [S1
rule 1.5] A move stops when a sowing ends in an empty pit (or at certain
special stops that do not apply in Kujifunza). After a move, the opponent
moves.

On each turn, a player's move is either a **mtaji** (capturing) move or a
**takasa/takata** (non-capturing) move. If any capturing move exists, the
player MUST make a capturing move. [S1 rule 1.8a]


## 5. Sowing Mechanics (Relay/Endelea)

[S1 rule 1.3, 1.6]

1. Pick up **all** seeds from the chosen pit (leaving it empty).
2. Drop seeds **one by one** into consecutive pits along the chosen direction.
3. Check where the last seed lands:
   - **If the last seed lands in an EMPTY pit:** the sowing ends. (But see
     capture check for the initial sow, Section 6.)
   - **If the last seed lands in a NON-EMPTY pit AND no capture is
     triggered:** pick up ALL seeds from that pit (including the one just
     dropped) and continue sowing from the NEXT pit in the same direction.
     This is called *endelea* (relay). [S1 rule 1.6]
   - **If the last seed lands in a NON-EMPTY inner-row pit AND a capture IS
     triggered:** execute the capture (see Section 6).

The relay can chain many times in a single turn.

**Direction is fixed within a sowing sequence.** During sowing, the direction
cannot change. [S1 rule 1.3: "During sowing, the direction of the sowing
cannot change."] Direction CAN change between sowings within the same move --
specifically, when a capture resets sowing from a kichwa.


## 6. Captures (Mtaji)

### 6.1 When do captures trigger?

**ANSWER: Captures trigger during relay sowing as well, not only on the
initial sow.**

This is the single most critical rule. The evidence:

- [S1 rule 1.6c]: "Endelea stops if a capture is possible. The move continues
  with the capture."
- [S1 rule 1.8]: "If a move starts with a capture, then more captures can
  occur during endelea later on."
- [S5 (gambiter.com)]: "While the player is relay-sowing, if the last seed in
  any individual sowing is placed in a marker, a new capture occurs."
- [S6 (Wikipedia)]: Captures occur when sowing ends "in a non-empty pit at
  the opponent's front row that has an opposing non-empty pit" -- this applies
  during relay.
- [S8 (Polovynka)]: "If a last seed falls into a non-empty hole in the inner
  row and the opposing hole is also non-empty" -- capture occurs, including
  during relay.
- [S1 Section 5.5.2 p.94]: "If at the end of a sowing, a capture is possible,
  the captured counters are sowed immediately at the own side of the board.
  This second sowing can again result in a new capture followed by a new
  sowing."

**Precise rule:** At the end of EACH individual sowing (whether it is the
initial sow or a relay continuation), if the last seed lands in a non-empty
pit on the player's inner row, AND the directly opposite pit in the
opponent's inner row is also non-empty, a capture occurs. If the last seed
lands in a non-empty inner-row pit but the opposite is empty, relay (endelea)
continues normally. If the last seed lands in an empty pit, the move ends.

**Critical distinction between mtaji and takasa moves:** Captures are ONLY
checked during mtaji moves. If the move is a takasa (non-capturing) move,
captures never trigger, even if a sowing ends in a "marker" pit. [S1 rule
1.7]

### 6.2 What qualifies a move as mtaji vs. takasa?

[S1 rule 1.7]: "If a move does not start with a capture, then capturing is
not allowed at all during that move."

This means: A move is classified as mtaji if and only if the FIRST sowing of
the move results in a capture. If the first sowing does not produce a capture
(either ends in an empty pit, or ends in an inner-row pit with an empty
opposite), the entire move is takasa, and no captures can happen in any
subsequent relay.

**For Bao la Kujifunza in the mtaji stage** [S1 rule 3.4]: A capture move
must start from a hole on the front row or back row that contains more than
one but **fewer than 16 stones**. After spreading in a chosen direction, the
last stone must allow capturing. If a capture is possible, it is obligatory.

### 6.3 Capture execution

[S1 rule 1.4]

When a capture occurs:
1. Take ALL seeds from the opponent's opposite inner-row pit (the "captured
   pit"). This pit becomes empty.
2. Begin sowing the captured seeds from one of the player's own kichwas.

**Which kichwa?** This depends on where the capture happened:

- [S1 rule 1.4b]: "If the capturing pit is the left kichwa or kimbi, the
  sowing must start at the left kichwa. If the capturing pit is the right
  kichwa or kimbi, the sowing must start at the right kichwa."
  - Left kimbi = columns 1, 2. Right kimbi = columns 7, 8.

- [S1 rule 1.4c]: "If the capturing pit is not a kichwa or kimbi, then the
  direction can be chosen by the player if the capture is the start of a move
  in namua stage. Otherwise the existing direction of the move must be
  sustained."
  - In Bao la Kujifunza (mtaji stage only): if the capturing pit is in
    columns 3-6 (not kimbi), the direction of capture sowing must be the SAME
    as the current direction of the move. This means sowing starts from the
    kichwa that maintains the current direction.

- [S1 rule 1.4a]: "If the sowing starts at the left kichwa, the sowing
  direction is clockwise; if the sowing starts at the right kichwa, the
  sowing direction is anti-clockwise."

**Combined rule for capture direction in Bao la Kujifunza:**
- Capture at columns 1 or 2 (left kimbi) -> sow from LEFT kichwa (col 1),
  direction = clockwise. Direction MAY CHANGE from what it was.
- Capture at columns 7 or 8 (right kimbi) -> sow from RIGHT kichwa (col 8),
  direction = anti-clockwise. Direction MAY CHANGE from what it was.
- Capture at columns 3, 4, 5, or 6 (not kimbi) -> sustain current direction.
  If currently clockwise, sow from left kichwa (col 1). If currently
  anti-clockwise, sow from right kichwa (col 8).

### 6.4 Does the first captured seed go INTO the kichwa?

**ANSWER: YES. The first seed is placed INTO the kichwa pit itself.**

Evidence:
- [S1 rule 1.4]: "The sowing of captured stones must start at one of the own
  kichwas." The word "start at" means the kichwa is the first pit to receive
  a seed.
- [S1 rule 1.3]: "Every sowing (spread) has a starting pit, a number of
  stones to sow, a sowing direction, and an ending pit." The starting pit
  receives the first seed.
- [S5 (gambiter.com)]: "The first seed must be sown in a kichwa."
- [S10 (abstract strategy blog)]: "The captured seeds are sown in a new lap
  towards the center from the kichwa."
- [S6 (Wikipedia)]: Same.

The kichwa receives the first captured seed. If there are N captured seeds,
they go into pits kichwa, kichwa+1, kichwa+2, ..., kichwa+(N-1) along the
sowing direction. After placing all captured seeds, check the landing pit for
relay/capture as usual.

### 6.5 Can a capture be triggered FROM a kichwa pit?

**ANSWER: YES.** A kichwa is a pit on the inner row like any other. If the
last seed of a sowing lands in a kichwa pit (col 1 or col 8), that kichwa is
non-empty, and the opposite opponent inner-row pit is non-empty, a capture is
triggered. [S1 rule 1.4b] specifically describes what happens when "the
capturing pit is the left kichwa or kimbi" -- confirming kichwa can be a
capturing pit.

### 6.6 Mandatory capture

[S1 rule 1.8a]: "It is obligatory to capture, if possible."

If any legal move for the current player results in a capture (i.e., the
initial sow's last seed lands on a non-empty inner-row pit with a non-empty
opposite), the player MUST choose such a move. Non-capturing (takasa) moves
are only legal when no capturing move exists.


## 7. Non-Capturing Moves (Takasa/Takata)

[S1 rule 1.7, rule 3.5]

A takasa move is made when no mtaji (capturing) move exists.

### 7.1 Takasa constraints in mtaji stage

- [S1 rule 3.5a]: "If possible, the player must takasa from the front row."
  (Inner row has priority over outer row.)
- [S1 rule 3.3]: "Only holes that contain more than one stone can be played."
  (Minimum 2 seeds to begin sowing.)
- [S1 rule 3.5b]: "If the only filled hole on the front row is one of the
  kichwas, takasa cannot go in the direction of the back row (because the
  front row will be empty and the game is a loss)."
- If all inner-row pits have 0 or 1 seeds, the player may begin from the
  outer row.

### 7.2 Relay during takasa

**YES, relay (endelea) applies during takasa.** [S1 rule 1.7]: "During takasa,
the player keeps performing endelea until it ends (rule 1.6a/b)." If the last
seed lands in a non-empty pit, pick up and continue sowing. The move only ends
when the last seed lands in an empty pit.

**But NO captures during takasa.** [S1 rule 1.7]: "If a move does not start
with a capture, then capturing is not allowed at all during that move."
Marker pits (inner-row pits with non-empty opposites) are treated as ordinary
pits during takasa -- relay continues through them without capturing.

### 7.3 Direction during takasa

[S1 rule 1.7]: "During takasa, the direction of the move cannot change."

The player chooses a direction at the start of the move, and that direction is
fixed for the entire takasa turn. (Contrast with mtaji moves, where direction
can change when capture occurs at a kimbi.)


## 8. The >15 Seeds Rule

**ANSWER: This rule IS real. It is called the "fewer than 16 stones" rule.**

Evidence from [S1 rule 3.4]: "A capture move in mtaji stage must start from a
hole on the front or back row that contains more than one but **fewer than 16
stones**."

This means: a pit with 16 or more seeds CANNOT be the starting pit for a
capturing (mtaji) move. If a player's only possible moves involve starting
from pits with >= 16 seeds, those moves are automatically takasa
(non-capturing), even if the sowing would otherwise end in a capturing
position.

**Cross-reference:**
- [S5 (gambiter.com)]: "If the first sowing is from a pit that has more than
  15 seeds, the turn will always be 'takata' irrespective of whether the last
  seed falls in a marker or not."
- [S6 (Wikipedia)]: Same rule confirmed.
- [S8 (Polovynka)]: "If a player starts from a hole which has 16 seeds or
  more -- it's always a blank move."
- [S10 (abstract strategy blog)]: Rule 3.4.2: "It's not allowed to harvest
  starting a move from a pit with more than 15 seeds."
- [S9 (Scribd quick reference)]: "Players can harvest seeds from the front or
  back row if a pit contains more than one seed but less than fifteen."
  (NOTE: this source says "less than fifteen" rather than "fewer than sixteen"
  -- a minor discrepancy. The Donkers thesis [S1] is authoritative: the
  threshold is strictly < 16, i.e., max 15.)

**Rationale:** With 16+ seeds in a pit, sowing wraps all the way around the
16-pit loop and returns to the starting pit (or beyond), which creates
complicated and potentially infinite sowing situations. The rule prevents this
by forcing such moves to be non-capturing.

**For the solver:** When generating legal capture moves, exclude any starting
pit with >= 16 seeds. Such pits can still be used for takasa moves.


## 9. Sowing Direction Choice

**ANSWER: The player can freely choose left or right on the FIRST sowing of a
move, subject to constraints. Direction is then constrained for the remainder
of the move.**

[S1 rule 1.3]: The player chooses a direction at the start of a sowing.
[S1 rule 1.4c, 1.7, 1.8]: Direction constraints apply after the initial
choice.

### 9.1 Direction rules summary

**At the start of a move (first sowing):**
- The player picks a starting pit and a direction (clockwise or
  anti-clockwise). This is a free choice (subject to the constraint that the
  move must be legal -- e.g., if a capture is mandatory, the move must result
  in a capture).

**During relay (endelea) within a sowing:**
- Direction cannot change. [S1 rule 1.3]

**When a capture triggers a new sowing from a kichwa:**
- If capture at kimbi (cols 1, 2, 7, 8): direction is FORCED by the kichwa
  choice (left kimbi -> left kichwa -> clockwise; right kimbi -> right kichwa
  -> anti-clockwise). This CAN reverse the direction. [S1 rule 1.4b]
- If capture at non-kimbi (cols 3-6): direction must be SUSTAINED from the
  previous sowing. [S1 rule 1.4c]

**During takasa:**
- Direction cannot change at all during the entire move. [S1 rule 1.7]


## 10. Winning and Losing

[S1 rule 1.1, 1.2]

A player **loses** if:
1. Their **inner row (front row) is completely empty** at any point (even
   during a move). [S1 rule 1.2: "The game ends if (1) the front row of a
   player is empty (even during a move)"]
2. They **cannot make any legal move** on their turn. [S1 rule 1.2: "or (2)
   if a player cannot move."] This occurs when all pits have 0 or 1 seeds
   (cannot sow from a single seed). [S1 rule 3.3]

The game is won by the opponent of the losing player. **There is no draw in
Bao.** [S1 Section 5.5.2 p.95: "Since in bao draws are not possible..."]


## 11. Cycles and Never-Ending Moves

### 11.1 The problem

[S3] (Kronenburg et al., 2006) proved that never-ending relay sequences
(infinite sowing within a single move) exist in Bao. They found a concrete
example with a cycle period of 218 sowings. [S1 Section 5.5.2 p.94] also
states: "The existence of endless moves can be proven theoretically (Donkers,
Uiterwijk, and De Voogt, 2002)."

**Key fact from [S3]:** Never-ending moves can ONLY occur during non-capturing
(takasa) moves. During mtaji moves, captures break the cycle by removing seeds
from the opponent's side and resowing them from a kichwa.

### 11.2 The Donkers computer rule

[S1 rule 1.5a]: "**Infinite Moves (special computer rule).** A move can take a
long time and sometimes last forever. However, these infinite moves are
illegal. If no finite move is available, the game is lost for the player to
move. Because infinity of a move can be very tedious to prove, a move is
regarded as infinite if more than a previously designated number of stones is
sown."

**This is the authoritative rule for computer play:** Infinite moves are
declared illegal. If the only moves available to a player are all infinite,
that player loses.

### 11.3 The Kronenburg proposal

[S3] proposes two categories:
1. **Proven never-ending moves:** Detected by the "Mayer Test" (mathematical
   conditions on the board state).
2. **Suspicious never-ending moves:** Detected by exceeding a predefined
   sowing limit (e.g., more than 100 individual sowings in a single move, as
   used in [S1 Section 5.5.4 p.96]).

Both categories are treated as illegal.

### 11.4 Rule for the solver

**Adopted rule (following [S1] and [S3]):**
- A move is declared INFINITE (and therefore illegal) if the sowing sequence
  exceeds a threshold of individual sowings. [S1 Section 5.5.4 p.96] uses
  "sowing of more than 100 stones" as the threshold.
- If a player's only legal moves are all infinite, that player LOSES.
- **Repeated board positions across turns:** Bao has no draw rule. Since
  64 seeds on 32 pits with turn information yields a finite state space,
  and since the game has no draws [S1], if a position repeats across turns
  it means the players are choosing to cycle. For the solver, we treat
  repeated positions as draws (game-theoretic value 0) by convention, or
  use a maximum game length cutoff. This is a solver design choice, not
  part of the traditional rules.


---

## 12. Complete Move Procedure (Algorithm)

This section gives a step-by-step procedure for executing a single move, for
implementation in a solver.

### 12.1 Determine move type

1. Enumerate all possible (starting_pit, direction) pairs where starting_pit
   has >= 2 seeds.
2. For each pair, simulate the first sowing. If the last seed lands on a
   non-empty inner-row pit whose opposite is non-empty, the move is a
   potential MTAJI move. Exclude starting pits with >= 16 seeds from mtaji
   candidacy.
3. If any mtaji move exists, the player MUST choose one (mandatory capture).
4. If no mtaji move exists, the player makes a TAKASA move. Takasa must start
   from the inner row if any inner-row pit has >= 2 seeds; otherwise from the
   outer row.

### 12.2 Execute mtaji move

```
FUNCTION execute_mtaji(board, player, starting_pit, direction):
    seeds = board[starting_pit]
    board[starting_pit] = 0
    is_capturing_move = True
    current_direction = direction
    sow_count = 0

    LOOP:
        # Sow the seeds
        landing_pit = sow(board, player, starting_pit, seeds, current_direction)
        sow_count += seeds

        IF sow_count > MAX_SOW_THRESHOLD:
            RETURN INFINITE_MOVE  # illegal

        IF board[landing_pit] == 1:
            # Landing pit was empty before we dropped our last seed (now has 1)
            # Wait -- actually: landing pit now contains 1 seed total
            # (the one we just dropped). Move ends.
            RETURN board

        # Landing pit is non-empty (>= 2 seeds including the one just dropped)
        IF landing_pit is on player's inner row:
            opposite = get_opposite(landing_pit)
            IF board[opposite] > 0 AND is_capturing_move:
                # CAPTURE!
                captured_seeds = board[opposite]
                board[opposite] = 0

                # Determine which kichwa to sow from
                col = column_of(landing_pit)
                IF col in {1, 2}:  # left kimbi
                    kichwa = left_kichwa
                    current_direction = CLOCKWISE
                ELIF col in {7, 8}:  # right kimbi
                    kichwa = right_kichwa
                    current_direction = ANTI_CLOCKWISE
                ELSE:  # cols 3-6, sustain direction
                    IF current_direction == CLOCKWISE:
                        kichwa = left_kichwa
                    ELSE:
                        kichwa = right_kichwa

                starting_pit = kichwa
                seeds = captured_seeds
                CONTINUE LOOP  # sow captured seeds from kichwa

        # Non-capturing relay (endelea): pick up and continue
        seeds = board[landing_pit]
        board[landing_pit] = 0
        starting_pit = next_pit(landing_pit, current_direction)
        CONTINUE LOOP
```

NOTE on the sow function: `sow(board, player, starting_pit, count, dir)`
places one seed each into `starting_pit`, then the next pit in `dir`, etc.,
for `count` seeds total. It returns the pit where the last seed was placed.

**Important:** After a capture, the captured seeds are sown starting FROM the
kichwa pit itself (the kichwa receives the first seed). After endelea
(non-capture relay), the seeds are picked up and sowing starts from the NEXT
pit (not the landing pit itself, which was emptied).

### 12.3 Execute takasa move

```
FUNCTION execute_takasa(board, player, starting_pit, direction):
    seeds = board[starting_pit]
    board[starting_pit] = 0
    sow_count = 0

    LOOP:
        landing_pit = sow(board, player, starting_pit, seeds, direction)
        sow_count += seeds

        IF sow_count > MAX_SOW_THRESHOLD:
            RETURN INFINITE_MOVE

        IF board[landing_pit] == 1:
            # Landing pit was empty, move ends
            RETURN board

        # Relay: pick up and continue (NO capture check)
        seeds = board[landing_pit]
        board[landing_pit] = 0
        starting_pit = next_pit(landing_pit, direction)
        CONTINUE LOOP
```

Direction never changes during takasa. No captures ever occur.


---

## 13. State Representation

### What constitutes a complete game state?

A game state is fully determined by:
1. The number of seeds in each of the 32 pits (always summing to 64)
2. Whose turn it is (1 bit)

No hand seeds, no nyumba status, no move history needed for Bao la Kujifunza.

### Symmetry Reductions

**Symmetry 1: Player swap (180-degree board rotation)**
Rotating the board 180 degrees swaps P1 and P2 and reverses left/right within
each row. With turn swap, the position is strategically equivalent. This
halves the state space.

**Symmetry 2: Left-right reflection**
Reflecting the board left/right (column c -> column 9-c) preserves all rules:
kichwa pits swap, kimbi pits swap, capture direction rules swap accordingly,
sowing directions swap. Since the nyumba (col 5) has no special rules in
Kujifunza, left-right symmetry holds perfectly. This halves the state space
again.

**Combined: up to 4x reduction.**


---

## 14. Summary of Resolved Ambiguities

| # | Question | Answer | Source |
|---|----------|--------|--------|
| 1 | Capture timing: initial sow only or also relay? | **Also during relay.** At the end of each sowing (including relay), if last seed lands in non-empty inner-row pit with non-empty opposite, capture occurs. But ONLY during mtaji moves. | [S1] rules 1.6c, 1.8; [S5]; [S6]; [S8] |
| 2 | Never-ending moves within a turn | Infinite moves are **illegal**. Detected by sowing threshold. If only infinite moves available, player **loses**. | [S1] rule 1.5a; [S3] |
| 3 | Repeated positions across turns | No draw rule exists in Bao. For solver: treat as draw or use max game length. | [S1] 5.5.2; solver design choice |
| 4 | The >15 seeds rule | **Real.** Starting pit with >= 16 seeds cannot be used for a mtaji move; such a sow is forced takasa. | [S1] rule 3.4; [S5]; [S8]; [S10] |
| 5 | Sowing direction choice | Free choice on first sowing. During relay: fixed. During capture re-sow: kimbi forces nearest kichwa (may change direction); non-kimbi sustains direction. During takasa: never changes. | [S1] rules 1.3, 1.4b, 1.4c, 1.7 |
| 6 | Takata relay | **Yes**, relay applies during takata. No captures ever trigger. | [S1] rule 1.7 |
| 7 | Kichwa during capture re-sowing | First captured seed goes **INTO** the kichwa pit. Sowing starts at the kichwa itself. | [S1] rule 1.4; [S5]; [S10] |
| 8 | Capture from kichwa | **Yes**, kichwa can be a capturing pit. Rules explicitly handle this case. | [S1] rule 1.4b |
