---------------------------- MODULE FlagReadback ----------------------------
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Policy, MaxSteps, MaxDepth
VARIABLES item, requiredExclusive, expectedOther, lock, cache, frames, steps, last
vars == <<item, requiredExclusive, expectedOther, lock, cache, frames, steps, last>>
X == "Exclusive"
O == "Other"
Flags == SUBSET {X, O}
Empty == [kind |-> "missing", flags |-> {}]

Init == /\ item = Empty
        /\ requiredExclusive = FALSE
        /\ expectedOther = FALSE
        /\ lock = FALSE
        /\ cache = [valid |-> FALSE, flags |-> {}]
        /\ frames = <<>>
        /\ steps = 0
        /\ last = "init"

VisibleFlags == IF Policy = "nnint" /\ item.kind # "value"
                   THEN {} ELSE item.flags

Read == /\ steps < MaxSteps
        /\ cache' = [valid |-> TRUE, flags |-> VisibleFlags]
        /\ steps' = steps + 1
        /\ last' = "read flags"
        /\ UNCHANGED <<item, requiredExclusive, expectedOther, lock, frames>>

OutputFlags(requested) ==
  CASE Policy = "blind" -> requested
    [] Policy = "stickyall" -> requested \cup item.flags
    [] Policy = "lock" -> requested \ {X}
    [] Policy = "stale" -> requested \cup (cache.flags \cap {X})
    [] OTHER -> requested \cup (VisibleFlags \cap {X})

Write(kind, exclusive, other) ==
  LET requested == (IF exclusive THEN {X} ELSE {})
                   \cup (IF other THEN {O} ELSE {})
  IN /\ steps < MaxSteps
     /\ IF Policy = "stale" THEN cache.valid ELSE TRUE
     /\ item' = [kind |-> kind, flags |-> OutputFlags(requested)]
     /\ requiredExclusive' = (requiredExclusive \/ exclusive)
     /\ expectedOther' = other
     /\ lock' = IF Policy = "lock" THEN lock \/ exclusive ELSE lock
     /\ cache' = [valid |-> FALSE, flags |-> {}]
     /\ steps' = steps + 1
     /\ last' = <<"write", kind, requested>>
     /\ UNCHANGED frames

Enter == /\ steps < MaxSteps
         /\ Len(frames) < MaxDepth
         /\ frames' = Append(frames,
              [item |-> item, required |-> requiredExclusive,
               other |-> expectedOther, lock |-> lock, cache |-> cache])
         /\ cache' = [valid |-> FALSE, flags |-> {}]
         /\ steps' = steps + 1
         /\ last' = "enter child"
         /\ UNCHANGED <<item, requiredExclusive, expectedOther, lock>>

Return == /\ steps < MaxSteps
          /\ Len(frames) > 0
          /\ cache' = frames[Len(frames)].cache
          /\ frames' = SubSeq(frames, 1, Len(frames) - 1)
          /\ steps' = steps + 1
          /\ last' = "successful child return"
          /\ UNCHANGED <<item, requiredExclusive, expectedOther, lock>>

Revert == LET parent == frames[Len(frames)]
          IN /\ steps < MaxSteps
             /\ Len(frames) > 0
             /\ item' = parent.item
             /\ requiredExclusive' = parent.required
             /\ expectedOther' = parent.other
             /\ lock' = parent.lock
             /\ cache' = parent.cache
             /\ frames' = SubSeq(frames, 1, Len(frames) - 1)
             /\ steps' = steps + 1
             /\ last' = "failed child / TryInvoke rollback"

RejectWrite == /\ steps < MaxSteps
               /\ steps' = steps + 1
               /\ last' = "validation rejects write"
               /\ UNCHANGED <<item, requiredExclusive, expectedOther, lock,
                               cache, frames>>

Next == Read \/ Enter \/ Return \/ Revert \/ RejectWrite
        \/ (\E kind \in {"value", "deleted"},
               exclusive \in BOOLEAN, other \in BOOLEAN:
               Write(kind, exclusive, other))

HasExclusive == (X \in item.flags) \/ lock
ExclusiveRetained == requiredExclusive => HasExclusive
NoLeakedExclusive == HasExclusive => requiredExclusive
OtherUsesLastWrite == (O \in item.flags) = expectedOther
TypeOK == /\ item.kind \in {"missing", "value", "deleted"}
          /\ item.flags \in Flags
          /\ requiredExclusive \in BOOLEAN
          /\ expectedOther \in BOOLEAN
          /\ lock \in BOOLEAN
          /\ cache \in [valid: BOOLEAN, flags: Flags]
          /\ Len(frames) <= MaxDepth

Spec == Init /\ [][Next]_vars
=============================================================================
