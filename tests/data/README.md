# Replay fixtures

`cpu-stack.etl` is a 4-second native x64 profile trace compressed with
`xperf -merge -mergeonly -compress`. It contains CPU samples and stack events
and can be replayed without administrator rights.

Replay expectations are stored in `cpu-stack.expected.json`. The fixture was
captured on Windows build 26200; event counts are stable, while symbol names
are intentionally not asserted because PDB availability varies by machine.