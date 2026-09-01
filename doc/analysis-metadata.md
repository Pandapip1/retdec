# Binary analysis metadata

The command "retdec-decompiler --analysis-metadata FILE --stop-after analysis
INPUT" writes RetDec's recovered binary structure without continuing into LLVM
optimization or C generation. The output is intentionally owned by RetDec so
consumers do not need their own object-file parser, disassembler, or
control-flow/reference recovery.

The top-level schema is "retdec-analysis-metadata-v1". Version 1 contains:

- input path, byte size, SHA-256 content digest, architecture, bit width, and
  image base;
- file sections with virtual/file extents and permissions;
- imports keyed by image address and library, plus structurally recognized
  import thunks keyed by thunk and IAT addresses;
- exports with name, ordinal, relative virtual address, and virtual address;
- recovered functions and basic blocks, including CFG successor edges;
- decoded instruction addresses, sizes, mnemonics, text, and normalized x86
  operands;
- a compact, per-function value-flow slice rooted at imported-call return
  values;
- classified code, data, and import references.

Addresses are unsigned JSON integers. Signed immediate values and memory
displacements are signed JSON integers. Register values are lowercase
architecture register names, not Capstone enum values, so consumers are not
coupled to a Capstone ABI.

The input size and SHA-256 identify the exact bytes RetDec analyzed. Consumers
may therefore reject stale cached metadata even when an input is renamed,
moved, or replaced at the same path.

The writer runs directly after "retdec-decoder". This placement is part of the
interface: metadata describes recovered machine structure before later LLVM
passes rewrite the CFG or remove machine-instruction mappings.

RetDec may split one machine instruction into several translator-only LLVM
blocks. Such blocks have no machine address and are not emitted as metadata
blocks. Their CFG edges are flattened to the next recovered machine block,
including every target of a resolved LLVM switch. A translator block that
contains later decoded instructions is emitted at its first decoded address.
This preserves resolved indirect/jump-table reachability without exposing
RetDec's internal micro-CFG.

## Value flow

Each recovered function has a `value_flow` object containing `definitions` and
`ambiguous_merges`. A definition is identified by the address of the machine
instruction that produced it. Its `destination` is either a canonical register
alias family (for example, writes to `al`, `ax`, and `eax` all name `eax` in a
32-bit image), a frame/stack-pointer-relative stack location, or a terminal
memory effect. Memory destinations contain normalized base and index register
families, scale, signed displacement, and access width. The recorded size
remains the width of the actual write.

The emitted definitions are a demand-independent backward graph, compacted to
the slice transitively derived from imported calls. A `call_return` is a root
only when its direct target is an import address or a structurally recovered
import thunk. Its `call_target` therefore joins directly to the top-level
`imports` or `import_thunks` relation. Each subsequent definition has zero or
more `inputs`; an input's `definition` is another machine-instruction address
and its `role` describes how it participates:

- `value` for register copies and stack spill/reload values;
- `base` for a pointer's base-register definition;
- `index` for an indexed pointer's index-register definition;
- `operand` for another input to a machine-level transform.

Operations are `call_return`, `copy`, `stack_store`, `stack_load`,
`pointer_load`, `indexed_load`, `address`, `constant`, and `transform`.
Instruction operands remain the source of scale, displacement, and immediate
details for value-producing loads. `pointer_store` is a terminal effect rather
than an alias-analysis definition: its destination records base, index, scale,
displacement, and width directly; its inputs independently identify the
unambiguous `base`, `index`, and stored `value` definitions. An immediate
stored value is emitted as `stored_immediate`.

Only stores whose unambiguous base or index provenance descends from an
imported return are retained. A retained store includes the complete
unambiguous provenance of its address operands and stored value, even when an
operand itself does not descend from that imported return. This operand
closure does not seed other downstream effects, and every emitted `definition`
edge names another emitted record.

The analysis propagates reaching definitions across recovered CFG edges. At a
merge it emits an edge only when all incoming paths agree on the same
definition. Otherwise it stops that value chain and adds an `ambiguous_merges`
entry containing the location, candidate definitions, and whether any path was
undefined. Consumers must not choose a candidate or synthesize a phi node from
this entry. This fail-closed rule also applies to stack locations invalidated
by stack-pointer changes and to unsupported machine writes.

For example, a consumer can join an imported `call_return` to a later
`pointer_load` through its `base` input, through `stack_store`/`stack_load`
definitions across basic blocks, and onward through both the `base` and
`index` inputs of an `indexed_load`, without disassembling the input itself.

Version 1 currently emits structured operands for x86 inputs. Adding another
architecture requires a schema-version-compatible normalized operand encoding
or a new schema version; it must not expose decoder-library numeric enums.
