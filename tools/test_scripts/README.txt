Adaptoid Reverse Engineering                              Test Scripts
tools/test_scripts                          Adaptoid script source


                    SCRIPT TESTS FOR BOTH DRIVERS


Abstract

   Nine .ac scripts for the Adaptoid configurator, each covering one area.
   They
   are the consolidation of eleven throwaway scripts; the originals are on the
   VM share under scripts/ and are not worth keeping.

   Every script prints to NOTEPAD, one short line per controller button, and
   every value is self-verifying - the answers are things like Fibonacci numbers
   and 1+2+...+100 that a reader checks at a glance. Measured results from the
   original driver are in ../testing/test_results.txt.

   HOW TO RUN ONE: copy it into the guest's scripts folder beside
   wishd201.exe, select it in the configurator, focus Notepad, press the
   button. Open the DEBUGGING LOG first for anything involving faults.

   THE @-- PREFIX IS PART OF THE NAME, NOT DECORATION. The configurator
   parses a script file name as <game>--<field>--<field>.ac, and a
   leading @-- is the form that means NO GAME BOUND - a common script,
   shown in the UI as < common >. Rename one without it and the
   configurator does not start correctly.

Table of Contents

   1. Two Constraints That Shaped Every Script
   2. The Scripts
   3. Which Ones Crash The Original


1. Two Constraints That Shaped Every Script

   SCRIPTS MUST USE CRLF LINE ENDINGS. An LF-only script HANGS THE
   CONFIGURATOR - the window greys out, the process spins at 100% CPU, and it
   must be killed. It is not recoverable by restarting: the last-loaded script
   is stored and re-opened on launch, so it hangs again immediately. Recover by
   fixing the file's line endings before reopening.

   THIS IS A TRAP BECAUSE NOTHING ABOUT THE SYMPTOM POINTS AT LINE ENDINGS, and
   the obvious theories all have working counter-examples. The file is opened
   in text mode, so the CRT translates CRLF to LF and the scanner sees the SAME
   character stream either way - an LF file is not read as one long line, and
   the comment text is not what matters. What differs is byte offsets:
   cfg_ScriptHandlerScan drives its loop on crt_ftell positions, which report
   where a match ended ON DISK, while the scan itself proceeds by character.
   With CRLF a line boundary costs two bytes and one character; with LF it
   costs one of each.

   Every script the original author shipped is CRLF, so the arithmetic was
   never exercised the other way. A script written with a Unix tool - a bash
   heredoc, a redirect, anything that emits bare LF - produces a file that
   looks correct in every editor and wedges the application on sight.

   Hunting this by bisecting the comment block wastes a great deal of time and
   CONVERGES ON A FALSE ANSWER, because a generator that rewrites the file
   preserves its line endings: every variant stays LF and every variant hangs,
   which reads as "the header is the problem". The two variants that appear to
   pass only do so because their headers stop at the tag block. Check line
   endings FIRST, against a script that works:

       file @--faultcrash.ac @--yourscript.ac

   which prints "with CRLF line terminators" for a good one and says
   nothing about terminators for a bad one.

   NO HEXADECIMAL LITERALS. The configurator's compiler emits a POINTER instead
   of the value for any 0x constant - see ../../docs/ known-defects.txt Defect
   21. Decimal is unaffected. A test written with hex measures nothing.
   @--constants.ac exists to demonstrate this; everything else avoids it.

   AT MOST ABOUT 100 KEY EVENTS PER PRESS. The script event queue holds
   100 entries and drops the OLDEST on overflow, so a handler that types more
   loses its BEGINNING. One short line per press stays well inside it.
   @--bigreport.ac deliberately exceeds it to show the effect.

   A THIRD, for anything that forks: _fork refuses once the thread count passes
   30, and _kill leaks that count (Defect 8). RE-SELECT THE SCRIPT between
   attempts or a later press silently forks nothing and looks like a pass.

   A FOURTH: the thirty-thread cap bounds how many threads exist AT ONCE, not
   how many a script may create. Both drivers refuse _fork past thirty on the
   ready list. On the original the count leaked, so the refusal was permanent
   and the script was crippled for its lifetime; on the replacement the count
   is accurate, slots free as threads end, and a script can keep creating them
   indefinitely. Concurrency is bounded. THROUGHPUT IS NOT.

   That matters because the cost is per FAULT, not per live thread: each one
   allocates a globals snapshot, frees the previous snapshot and thread node,
   and may post an event. Under Driver Verifier special pool every one of
   those is a dedicated page. Handlers are driven from the input stream rather
   than from button edges, so a held button re-enters a handler that is still
   running and the invocations stack, which is how a hundred forks per press
   becomes thousands of threads.

   SO A SCRIPT MEASURING SOMETHING ELSE KEEPS ITS FORK COUNT SMALL AND GUARDS
   RE-ENTRY WITH A static FLAG, or the storm becomes the only thing it tests.
   @--racewindow.ac does neither, deliberately: overloading that path IS what
   it measures.


2. The Scripts

   +------------------+---------------------------------------------------+
   | Script           | What it covers                                    |
   +==================+===================================================+
   | @--arith.ac      | Integer semantics where two implementations may   |
   |                  | legitimately differ: negative division and        |
   |                  | modulo, arithmetic vs logical shift, shift counts |
   |                  | past 31, precedence, the conditional operator.    |
   |                  | Pure logic, no faults. The replacement matches    |
   |                  | all six.                                          |
   | @--constants.ac  | Defect 21. The same literal in different places   |
   |                  | compiles to different pointers, and size is       |
   |                  | irrelevant - a four-digit constant fails as badly |
   |                  | as a full 32-bit one.                             |
   | @--bigreport.ac  | A large script, over 2000 opcodes, that types a   |
   |                  | long self-verifying report. Nested loops,         |
   |                  | for/while/do, break, continue, static storage     |
   |                  | across presses, a four-deep call chain. The only  |
   |                  | script that overruns the 100-event queue, so the  |
   |                  | first lines of its report are LOST BY DESIGN.     |
   | @--threads.ac    | The thread primitives. Defect 8: forty fork+kill  |
   |                  | pairs, counting refusals - the original refuses   |
   |                  | 15, then 40 on a second press; the replacement 0. |
   |                  | Defect 7: a forked child that reads a PARAMETER   |
   |                  | is refused its own locals and faults with status  |
   |                  | 2.                                                |
   | @--edgemath.ac   | Operand values a divide cannot compute. The       |
   |                  | interpreter checks the divisor against zero       |
   |                  | and nothing else, so INT_MIN / -1 reaches a       |
   |                  | bare idiv and overflows - the same exception      |
   |                  | as divide by zero, raised in a DPC. Also          |
   |                  | recursion past the stack clamp and whether a      |
   |                  | script recovers after the budget runs dry.        |
   | @--faultcrash.ac | THE IMPORTANT ONE. Enumerates the interpreter     |
   |                  | fault codes, and crashes the original driver on   |
   |                  | ONE PRESS. See section 3.                         |
   | @--racewindow.ac | DELIBERATE OVERLOAD. Unguarded handlers that      |
   |                  | fork a hundred faulting threads a press and       |
   |                  | stack when the button is held, driving the        |
   |                  | fault path past what a client can drain.          |
   |                  | Passes when events_dropped stays ZERO and the     |
   |                  | driver stays up, however hard it is driven.       |
   | @--reload.ac     | The THIRD claim on the fault slot: a script       |
   |                  | load landing while a fault is in flight.          |
   |                  | Faults are delayed 3 or 6 seconds so there is     |
   |                  | a window to switch scripts in. Also forking       |
   |                  | from inside a call chain, and eight threads       |
   |                  | driving the HID report path at once.              |
   | @--stuckkey.ac   | Holds a key for as long as a button is held.      |
   |                  | Switch script while the button is down and the    |
   |                  | key is stranded - the driver keeps it pressed     |
   |                  | forever. Uses F13 so a stranded key does not      |
   |                  | fight the desktop.                                |
   +------------------+---------------------------------------------------+


3. Which Ones Crash The Original

   @--faultcrash.ac, button A. ONE PRESS from a freshly loaded script.

       1. arm Driver Verifier special pool on the DRIVER FILE:
              verifier /flags 0x1 /driver wishk201.sys
          and reboot. NOTE THE NAME - wishk201.sys is the file,
          wishna1k is only the service, and verifying the service name
          silently does nothing.
       2. plug the adapter in, start the configurator
       3. OPEN THE DEBUGGING LOG. No drain without it, and no drain
          means no race.
       4. select @--faultcrash.ac
       5. press A

   Button A forks eight children which each divide by zero, so all eight fault
   in one scheduler pass while the configurator drains after every fault
   notification. Each fault frees the snapshot the drain is still copying. That
   is Defect 13.

   Seven crashes by seven routes all landed on the same two instructions,
   drv_IoctlDeviceCommand at ghidra 0x133f0 and 0x133fa.

   WITHOUT VERIFIER IT DOES NOT STOP FOR THE DEBUGGER, it reboots outright. Arm
   special pool before trying to debug it.

   @--stuckkey.ac strands a key rather than crashing, and @--threads.ac disables
   _fork
   for the life of the script. Neither is a crash but both are user-visible
   failures.
