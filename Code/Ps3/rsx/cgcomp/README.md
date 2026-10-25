# cgcomp assembler (from PSL1GHT)

The NV40 vertex/fragment program assembler from PSL1GHT's `tools/cgcomp`
(commit f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe), MIT licence (`LICENSE.PSL1GHT`).
Only the assembly path is used; `../rsx_asm.cpp` holds the output code from cgcomp's
`main.cpp`. `tools/rsx-asm-test/run.sh` checks it writes the same bytes as `cgcomp -a`.

Changes for running inside the game:

- `exit(EXIT_FAILURE)` throws `CgcompError` instead (`types.h`), caught in `rsx_asm.cpp`.
- `fpparser.cpp`: an unknown destination register is an error. It used to leave the
  register index uninitialised, so the assembler wrote outside its register tables.
  This is what a named `TEMP` does: the fragment parser skips `TEMP` declarations, so
  fragment programs must use numbered registers (`R0`, `H1`...).
