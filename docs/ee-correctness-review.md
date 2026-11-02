# EE correctness review and GT4 regression repair

The broad audit candidate was withdrawn after actual GT4 runs regressed. Review
also established that the latest committed discovery restrictions had not been
regenerated into the previously working GT4 executable. Regenerating that commit
reproduced the early missing-callback stop. An old rendered executable therefore
was not evidence that the current converter still preserved startup.

The retained repair targets callback discovery. It preserves existing descriptor
candidate behavior and leaves the broader local alias/stack rewrite for separate
work. Rejected candidates and private commercial diagnostics remain under ignored
`work/`; none are commercial test fixtures.

## Retained changes

| Item | Status | Implementation and acceptance |
| --- | --- | --- |
| GPR effects blocking callback setup recovery | FIXED for the encountered family | `pointer_flow.hpp` classifies low-GPR writes. Known operations which leave a pointer register untouched no longer terminate its constant slice. Unsupported value transformations remain unresolved. |
| Earlier-block callback setup and loads | FIXED, bounded | Ordinary visits and delay-only visits are distinct. Predecessor slices preserve delay-slot and call-clobber behavior, and require an agreed scalar constant within their budget. |
| Indexed registration helpers | FIXED for the reproduced pattern | Match the descriptor pointer slot, record stride and called field to a bounded indirect consumer. Native synthetic execution returns the callback result; adjacent context and unsupported payloads are not classified as callbacks. |
| Nested completion-record registration | FIXED for the reproduced pattern | Relate an object stored into a completion record to its callback field, using an observed two-load indirect consumer. The generated native fixture executes the callback, rejects an unrelated field and leaves the context payload uncompiled. This supplies compilation candidates, not immutable runtime values. |
| Reference-taking function/context setters | FIXED for the reproduced pattern | Recover a temporary callback word passed by reference, including a call-delay store and an ordinary conditional branch that skips registration. Require an observed indirect consumer that passes the adjacent context field, and a setter that binds both fields. The native fixture returns the callback result; an unrelated context-field layout remains unclassified. Other stack alias forms remain unresolved. |
| Saved stack thread descriptors | FIXED for the reproduced pattern | Bounded frame-relative analysis follows preserved register aliases, calls and retry branches. The synthetic native program creates and starts the discovered thread and executes its entry. Partial and complete writes through an alias invalidate the earlier entry candidate. |
| Copied function/context records | FIXED for the reproduced families | Recognize complete aligned 64-bit copies and matching left/right load-store pairs in helpers and inline code, including a computed destination, preserved stack aliases and a displaced consumer base. Reuse the stack-reference analysis across intervening calls. Native fixtures execute the callback while adjacent opaque context, incomplete copies and unrelated fields remain data. These are compilation candidates, not proofs of immutable storage or general object types. |
| Left-partial writes invalidating pointers | FIXED for the reproduced family | Bound the possible bytes before the nominal address of SWL and SDL when base alignment is unknown. A regression previously rediscovered a callback after such a store cleared it; both widths now reject the stale pointer. The conservative bound can leave valid but unproved cases unresolved. |
| Countdown callback-table walks | FIXED for the reproduced pattern | Prove a common counter decrement on every forward path, including a branch-likely null-skip, and a bounded cursor stride at the back edge. Match indexed registration to the resulting table, including a callback store in a return delay slot. The native fixture executes both populated records and skips the empty record; inconsistent counter paths remain unclassified. |
| Object callbacks forwarded into indexed records | FIXED for the reproduced families | Match a function/context setter and its indexed destination to an observed indirect consumer, including context loads before the function load and loads separated by a null check. Recover callbacks supplied directly as arguments or read from the context object's field through forwarding helpers. Native fixtures execute both forms. Negative fixtures reject unrelated fields/contexts and definite full, partial and local-alias overwrites; a pointer loaded before a later field clear remains usable. The analysis is bounded and does not establish general interprocedural alias completeness. |
| Callback records carrying saved GP and user data | FIXED for the reproduced family | Require a consumer that restores GP and loads an opaque argument from the callback record, then match the registering helper's function, GP and context stores on one bounded control-flow path. Native execution uses the saved GP and user data. Mismatched fields, partial overwrites, reused base registers and mutually exclusive stores remain unclassified; adjacent payloads remain data. |
| Callbacks inside registered user-data objects | FIXED for the reproduced family | Follow a known registered callback's callable-field accesses back to the argument supplied as its context. This provides a concrete consumer for the caller's object stores, including taken branch-likely delay slots. The native fixture executes the inner callback; an unrelated context, different field, definite overwrite or annulled store does not establish a target. |
| Mutable indirect execution | VERIFIED OK for the fixture | The compiled fixture calls two successive values from a writable slot, then stops explicitly at an unknown target. No runtime instruction decoder or success stub is introduced. |
| Shared native EE test fixture generation | FIXED | One CMake target owns the LLVM fixture object, with dependencies from all three native test consumers. The parallel build generates it once; related EE, VU and SIF tests pass. |

## Remaining checklist items

| Item | Status | Remaining evidence |
| --- | --- | --- |
| Every callback or dynamically constructed target | UNRESOLVED | Bounded analysis is incomplete; unknown targets must keep producing diagnostics. |
| False executable discovery in arbitrary descriptors | UNRESOLVED | Existing descriptor candidate scanning remains. Tightening it needs separate evidence that legitimate registration paths are preserved. |
| Aliased/overlapping writes and stack argument coverage | UNRESOLVED | Existing tests cover cases, but the broad proposed rewrite was withdrawn. General interprocedural alias and predicate analysis is not established. |
| Complete GPR-read, ISA, ELF layout, code protection and LLVM ABI audit | UNRESOLVED | Existing behavior tests are not an exhaustive audit. |
| Reference trace comparison | UNRESOLVED | No new comparable commercial reference trace was collected. |
| All configured tests passing | UNRESOLVED | The earlier full Release run passed 32 of 33 tests. The opt-in local-startup test used an unchanged older diagnostic executable and differed from configured exact operation/interrupt counts. That expectation was not weakened. |
| Complete GT4 startup regression repair | UNRESOLVED | Regenerated native output preserves the warning-screen pixels, reaches thirteen EE threads and loads through RTINETB. Registered-context and inline-record callbacks now execute. The latest visible run stops at another callback passed through a reference-taking wrapper. It has not regained the earlier LIBSD key-on boundary. |
| GT4 title screen and gameplay | UNRESOLVED | Verified rendering is startup output. Missing code discovery and the previously encountered LIBSD key-on/sample-playback service still require work. |

Private checkpoints record generated projects, build results, runtime boundaries
and captured graphics separately. Actual GT4 validation is required in addition
to the synthetic tests. No interpreter, JIT, BIOS dependency, fake service
completion or game-specific runtime rule was added.
