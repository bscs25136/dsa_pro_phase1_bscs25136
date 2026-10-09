# Time-Travel Debugger — Phase 01 Progress
# Progress

# Project Setup

- Created the project and GitHub repository.

- Added progress.md for tracking project progress.


Stage 0 — Receive

- Studied the input source.bin format.

- Implemented a custom templated Stack class for maintaining the live call stack during execution.

- Implemented a doubly linked list based Timeline for storing execution snapshots.

PASS 0x0 — Source Validation

-  Implemented readSourceLine()
-  Implemented firstWord()
-  Implemented secondWord()
-  Implemented validateProgram()
-  Reads source.bin
-  Uses a stack to track active function declarations

 Current Status - 2 October 2026

-  PASS 0x0 validation has been implemented

 In Progress

-  PASS 0x1 — Resolve
-  PASS 0x2 — Execute
-  PASS 0x3 — Serialize 

 4 October 2026 — Resolve

 PASS 0x1 — Resolve

- Implemented writeResolveRecord().
- Implemented readResolveRecord().
- Implemented resolveProgram().
- Implemented storing every source line in resolve.bin
- Implemented calculation of byte offsets for resolve records.
- Implemented function tracking using FuncEntry.
- Implemented tracking of unresolved function calls using PendingPatch.
- Implemented detection of the main function and its offset.

 Current Status

 Completed

- Project setup
- PASS 0x0 — Source Validation
- PASS 0x1 — Resolve
- Custom Stack
- Timeline data structure

- In Progress

- PASS 0x2 — Execution

- Not Started

- PASS 0x3 — Serialization

- 7 October 2026 — Resolve
- Working on execution - working on the logoc of call and func_end

- 9-10 Oct
- worked on execution but still some implementation is missing as for now ive just implemented my artithematic funcs - assuming second operand is js a number 
- will implement its code for it being a var tom - (add a b)
- will also work on serialization part tom
- ill and alr did some assumptions as no exact structure of source specified
