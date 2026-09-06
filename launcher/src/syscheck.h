#pragma once
// Built-in hardware check. Exercises every chip on the board, prints one
// "[syscheck] <name>: PASS|WARN|FAIL - <detail>" line per step to the serial
// console and mirrors them on the panel, then "[syscheck] done: ..." which
// tools/flash.py waits for. `interactive` adds the touch and button steps that
// need a human (skipped over a bare serial session).
namespace syscheck {
void run(bool interactive);
}
