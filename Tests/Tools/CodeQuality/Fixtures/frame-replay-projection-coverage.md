# Frame replay projection coverage

`FrameReplayFixture.cpp` observes real production owners after scheduler settlement.
`FrameReplayProjection.h` reads explicit, typed public members of sealed records.
The receipt compares semantic values and order, not whole C++ object bytes.
Both tools must be compiled against the complete headers matching their loaded DLL.

| Published domain | Authoritative receipt coverage |
| --- | --- |
| Renderer root and receipt | Frame, session, scene, publication and predecessor generations; outcome and page/view/range receipts. |
| Persistent topology | Every page, slot, authored object/mesh/material ID, lifetime key, resource generation, route and packet identity, in producer order. |
| Dynamic state | Every lane, exact current/previous matrices and canonical pose, instance selectors, animation generation, complete published animation binding, CPU deformation owner contents, shadow caster stamp and all generation/occupancy fields. |
| View selection and commands | Output identities, visible keys, LOD/routes/packet/tint selectors and tint values; every resolved command field, including animation binding, shadow stamp and payload selectors; submission ranges and resource preparation records. |
| Reference frames | Every canonical cell/offset and frame/view/output/origin/spatial/residency generation. |
| Presentation inputs | Every named value member: output/camera matrices and origin, settings, efficiency controls and oracle bytes, light payloads and metadata, sky, grouped instances/submissions, command lanes and selections, UI runs, shadow lists and prepared instance rows, all virtual geometry packet/batch/frame-set/compaction/generated/counter/fallback/retirement/decoded-upload evidence. Retained const storage is traversed without replacing it with a count or digest. |
| Animation backend work | Frame validity/rejection, owner ranges and generations, complete immutable GPU animation plan, resident program/clip/skeleton/mask/morph bytes, generation/work/render-identity/dispatch/batch/stage records and retained static/dynamic lanes. |
| CPU deformation | Every record and owner/generation/content identity, all palette matrices and morph weights. |
| GPU page deformation | Complete page/deformation keys, work and ordered consumer records, source/output spans, history flags, view masks, totals and validation/rejection values. This is the actual sealed plan, not a claim that GPU output was read back. |
| Animation authority | Every page and row, receipt, authored and runtime identities, policy/status values, source transform, complete snapshot/playback/parameter/pose values, ordered channels/events/root motion and every specialist sidecar. Known baseline compact-row metadata omissions remain unchanged in the projection. |
| Static skeleton ownership | Loaded asset ID/runtime generation, compatibility fingerprint, reference-pose hash, root/version and joint IDs; static cooked bytes are covered by the verified project file closure. |
| Scheduler, visibility and lifetime | Actual declarations/resources/phases/dependencies/results, full-engine graph summary, real typed transaction ingress/receipt/visible-state observations, producer callback causality and held-root/lifecycle predicates. The two execution lanes remain separate. |

All floating values use their exact IEEE words, including signed zero. Vectors and
arrays retain production order. Optional/shared values retain their actual absence
or const contents; ownership cycles fail the collector. No generic unknown-type
fallback substitutes counts, hashes, source text or a parallel execution model.
Only the previously reviewed receipt paths allow explicit bijective session-ID
mapping. Newly included nested session/options/backend generations remain exact;
an actual difference requires separate semantic-domain review before mapping.

The typed presentation/backend inventory currently covers 86 record types and 813
non-static value members: 808 authoritative members and five timing fields copied
under diagnostics. Changes to those production records require a
projection/ledger review; compilation alone cannot detect a newly omitted member.

| Excluded or separate data | Reason and remaining proof |
| --- | --- |
| C++ padding, container capacities, addresses, private allocator bookkeeping and `use_count()` | They are physical storage details, not sealed output values. The collector never emits or compares them. Explicit published capacity/budget fields remain covered. Pointer comparisons support the named warm-topology ownership predicate, ownership-cycle detection, and restart inequality against a still-retained old publication root. |
| Memory-lease implementation internals and scratch backing bytes | Not exposed as published semantic data. Pose lease presence, pose layout, copied scratch pressure/reclamation facts and held-root/clear/shutdown observations remain explicit; separate production lifetime tests cover allocator settlement. |
| Repeated static skeleton bind tables/source labels and other original cooked asset files | Verified actual project files provide SHA256/size evidence; loaded identity/generation/content proofs bind the skeleton owner. Derived resident GPU payload bytes and CPU deformation values are still included. |
| Wall-clock timings, physical thread/module paths and raw independent sibling completion order | Stored under the explicitly separate diagnostic/provenance fields. Raw callback order must pass pairing, partition and dependency checks before its canonical fingerprint can qualify. Published arrays are never reordered. |
| Published timing fields | The four `RenderOptimizationSummary` nanosecond fields and `VirtualGeometryCounterReadbackRecord.latencyFrames` are copied exactly to `diagnostic.timings`, joined to actual frame/view/output and counter identity. Availability, validity, reasons and all counter values remain authoritative; asynchronous differences in those values are not masked. |
| Native GPU completion/image equality | Outside this stage collector's claim. It observes real engine execution and sealed plans; the installed readback/image campaigns provide their own native evidence. |

`no-output` is the sole intentional renderer-root absence: the unchanged baseline
returns before publication/renderable construction. Both lanes must instead show
actual empty camera capture, empty captured renderables, separately labeled typed
scene mesh inputs, a null publication, current no-view diagnostics, invalid Scene/
Game output stats and completed scratch work. No missing stage data is invented.

The producer boundary lane registers the unchanged seven-system native dependency
closure: Scripting, Animation, Physics, AnimationPresentation, UI, virtual geometry
streaming and RenderSystem. Engine owns their initialization and shutdown. Schedule
validity and typed diagnostics are authoritative before ingress and each measured
plan must retain the required producer descriptors, including deliberately skipped
results after faults. Native compatibility Update calls are observed inside the
scheduler-created fallback, then joined to its unique actual single-partition
result before causality checks. This lane still flushes only its scoped scene
queue; FullEngine remains the evidence for the maintained complete engine flush.

The scoped provider callback acknowledges Physics Transform receipts and delivers
managed Physics events at the maintained After Physics/Frame End consumer predicate.
The receipt records actual callback invocation, fixed ticks and supplied receipt
counts; it does not infer private pending-step completion from those facts.

Before-exchange cancellation uses public evaluation-generation ingress, not graph
context cancellation. Both matched runtimes reject the unready authority token at
FinalizeAuthorityGate: the first command must be Rejected with publication zero.
The exact receipt and public cache counts before/after ingress and callbacks remain
observable; counts do not prove individual cache membership. Restart additionally
asserts semantic immutability of both initially held and pre-shutdown roots after
shutdown and the fresh session, without asserting pointer identity to new roots.

Restart renderer identity is publication lifetime, not globally monotonic backend
counters. The native backend resets its local recovery generation on a new context.
The collector requires empty renderer/animation stores and absent context after
shutdown, then a live context and publication distinct from the still-retained old
root. Exact old/new numeric domains and full fresh roots remain authoritative;
no general address-reuse or cross-instance generation uniqueness claim is made.

Profile publication identity separates nativePublicationGeneration (root.generation)
from nativePublicationReceiptGeneration (the embedded receipt value). Original
publications can retain default zero receipt metadata despite a live nonzero root
generation; both actual values remain exact and no native repair is implied.

Multi-output preserves repeated MainRuntimeView/output2 records: the native runtime
submission loop emits one ordered view per captured authored camera. Each measured
frame joins the two distinct authored camera IDs and protected active Transform/
Camera values to the corresponding ordered presentation matrices, plus the Scene
override and submission-range view indices. The association follows captured-camera
order and native submission order; no per-view camera UUID is published, and equal
camera payloads do not establish unique camera identity by matrix value alone.

Recording each canonical pose and ordered camera association does not establish
individual per-camera positional localization through cameraLocalPosition or
referenceFrame. Native camera-relative matrices may coincide. The complete actual
published reference frames, camera positions and matrices still compare exactly.
