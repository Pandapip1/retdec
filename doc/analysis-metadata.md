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
- recovered functions and basic blocks, including CFG successor edges;
- decoded instruction addresses, sizes, mnemonics, text, and normalized x86
  operands;
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

Version 1 currently emits structured operands for x86 inputs. Adding another
architecture requires a schema-version-compatible normalized operand encoding
or a new schema version; it must not expose decoder-library numeric enums.
