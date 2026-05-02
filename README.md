# Project SoftBounded

This project has a simple-sounding goal: **make [SoftBound](https://acg.cis.upenn.edu/papers/pldi09_softbound.pdf) usable in security-critical legacy or ultra-low-level C projects.** As a context, [SoftBound](https://acg.cis.upenn.edu/papers/pldi09_softbound.pdf) was a paper published in 2009 (more than a decade ago). The paper describes the basic design of enforcing buffer boundaries in C source code at compile time. Even though the paper proved that boundary checking is complete when done right, no real projects (that I'm aware of) use it, and _still, C pointers are bare_. How come? This project aims to address the "practicality" problems in SoftBound so that security-critical legacy or ultra-low-level C projects are no longer _bare_ in their memory pointers.

## Motivation: Why Now?

If you asked, "Why are we looking at a (soon-to-be-)multi-decade-old paper now?", I'd have asked the same question if it were not my project! In short, let me summarize the main motivations of this project below:

 - Despite all memory-safe languages, ultra-low-level programs _still_ need direct pointer access and manipulation. ([Longer version](https://gwangmu.medium.com/turn-key-solution-for-ultra-low-level-c-source-code-part-1-2aeb99153d84))
 - In the era of (so-called) AI security, _not_ having any memory vulnerabilities became more important than ever. ([Longer version](https://gwangmu.medium.com/turn-key-memory-safety-solution-for-ultra-low-level-c-source-code-part-7-b91fe08f7349))

I'll just add some contextual information here. By "ultra-low-level" (which I don't think is a mainstream term because I just made it up), I meant programs that directly interface with hardware. Those programs _need_ direct access to and manipulation of memory pointers because that's the only way to interact with hardware. This is the unsalvageable point for memory-safe programming languages. See [this article](https://www.usenix.org/system/files/1311_05-08_mickens.pdf) about this point.

One more thing: [a report from Anthropic about vulnerability-exploiting AI agent](https://red.anthropic.com/2026/mythos-preview/?mkt_tok=Mjk4LVJTRS02NTAAAAGhFc8YktS1OcD1FeJD61gGftks2gr-8mR8n9qc0PY6_8NFko-U9ezan7ANXq6db2IbqTc2K32V9wwF3djulAf44-9-d5_ht-9V5hNrCbchXVU4jeFe28Y) suggests that most discovered bugs were still memory corruption bugs, meaning that if we can somehow nip the memory corruption bugs from the root, the crisis in security could be mitigated a lot. On a side note, it's interesting how they also pointed out the "ultra-low-level" pointer operations at one point in this report (although the wording was not "ultra-low-level," obviously).

## Objective and Requirements

Talking about enforcing memory safety (not just SoftBound), I think it'll definitely benefit the **_security-critical_ legacy or ultra-low-level programs**, such as the control systems for energy infrastructure, weapons, airlines, or banking, and by being _security-critical_, the security requirements outshines some performace disadvantages (almost all security measures cost some performance overheads, but I imagine security-critical programs may have a higher tolerance on that front). For those programs, a solution that is just applicable with "one click" (well, maybe a few clicks, to be fair) could be enough, as long as the performance overhead is tolerable. ("Tolerable" is ambiguous wording, I know.)

I thought SoftBound's approach might be a good fundamental solution, but we should admit that it's considered impractical in practice. Why is it that? I think the reason is largely twofold.

 - (Obviously) performance overheads.
 - Brittle security guarantee. ([Longer version](https://gwangmu.medium.com/turn-key-memory-safety-solution-for-ultra-low-level-c-source-code-part-5-dc5e9c6ec539))

By "brittle," I mean that SoftBound's security guarantee is too easily broken by too many factors: usage of external libraries, (possible) imperfection in static analysis, no support for temporal memory corruption, ... There may not be a "silver bullet" solution for this, and it can very much be a whack-a-mole problem (like any other engineering problem). If you embrace the fact that this may require some step-by-step, continuous improvements, this strategy may make sense: **improve the brittleness of SoftBound in real-world programs while (also) attempting to mitigate performance overheads.** (Sounds a lot similar to what [WineHQ](https://www.winehq.org/) is pursuing for compatibility, eh?)

## Design

The design is changing dynamically (as of now, [this article](https://gwangmu.medium.com/turn-key-memory-safety-solution-for-ultra-low-level-c-source-code-part-5-dc5e9c6ec539) is closer to what it is right now), and once some functionality standards are met, I'd like to add optimizations to improve performance. For now, my pity attempt to make the performance better is i) to trade memory overheads for performance overheads (=prefer using more memory if it can help runtime performance), and ii) to _directly_ add runtime logic to target code so that the compiler can optimize both of them at once (in the hope that compilers remove unnecessary memory safety logic on behalf of me).

## Agenda

The agenda items below are tentative, and the earlier items may be revisited iteratively while the latter are being conducted.

 - [X] Write a basic runtime logic.
 - [ ] Write an instrumentation IR pass _after optimization_.
    - [ ] Intra-function parts.
    - [ ] Inter-function parts.
 - [ ] Investigate instrumented IRs and check runtime functionality and overheads.
    - [ ] Unit-test code.
    - [ ] Tiny open-source code. (~1k loc)
    - [ ] Small open-source code. (~10k loc)
 - [ ] Set up fuzzing pipelines and observe performance overheads.
    - [ ] (Benchmark projects TBD)
    
## Notes

 - Contributions are welcome! (Even though almost nothing has been done or workable other than by me yet...)
