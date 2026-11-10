# QuickEIP writing style

This guide gives the writing rules for QuickEIP documentation. The
*QuickEIP Programming and Implementation Manual* (`docs/manual/`) uses them.
Use them for new documentation and for changes to it.

Two standards apply:

- **ASD-STE100, Simplified Technical English (STE)**, issue 8, for the text:
  words, sentences, procedures, and safety notices.
- **The Chicago Manual of Style (CMOS)**, 18th edition, for the apparatus:
  title capitalization, punctuation, numbers, captions, notes, and the
  bibliography.

If the two standards disagree, STE controls the text, and Chicago controls
the format.

## Contents

- [1. Sentences](#1-sentences)
- [2. Words](#2-words)
- [3. Procedures](#3-procedures)
- [4. Safety notices](#4-safety-notices)
- [5. Descriptions](#5-descriptions)
- [6. Titles and headings](#6-titles-and-headings)
- [7. Punctuation](#7-punctuation)
- [8. Numbers and units](#8-numbers-and-units)
- [9. Tables, figures, and equations](#9-tables-figures-and-equations)
- [10. Notes and bibliography](#10-notes-and-bibliography)
- [11. QuickEIP terms](#11-quickeip-terms)
- [12. Test status](#12-test-status)
- [13. Markdown and LaTeX forms](#13-markdown-and-latex-forms)
- [14. Checklist](#14-checklist)

## 1. Sentences

| Rule | Limit or form |
|---|---|
| Procedure sentence | 20 words or fewer. |
| Description sentence | 25 words or fewer. |
| Paragraph | One topic. 6 sentences or fewer. |
| Voice | Active. "The adapter refuses the open," not "The open is refused." |
| Tense | Simple present, simple past, or simple future. |
| Contractions | None. "Do not," not "don't." "It is," not "it's." |
| Articles | Keep "a," "an," and "the." Do not write in telegraph style. |
| `-ing` forms | Do not use them as nouns or as adjectives where a simpler form exists. |
| Lists | Introduce each list with a full sentence that ends in a colon. |

A technical name does not count against a limit and is never changed.
Technical names include function names, file names, macros, CIP services
(Forward_Open), and product names.

Examples:

| Do not write | Write |
|---|---|
| It's recommended that the session be closed before it's reopened. | Close a session before you open it again. |
| Calling `eip_adapter_poll` on an RTOS port should be avoided. | Do not call `eip_adapter_poll` on an RTOS port. |
| The adapter, which will retry every second if the network isn't up, listens on three ports. | The adapter listens on three ports. If the network is not ready, the adapter tries again each second. |

## 2. Words

Use one word for one meaning, and use it the same way each time. Use the
simplest common word. The table lists the substitutions that occur most in
this project.

| Do not use | Use |
|---|---|
| ensure, verify, confirm | make sure |
| utilize, leverage | use |
| should (for an instruction) | must, or an imperative verb |
| should (for a probability) | can, or state the fact |
| prior to | before |
| in order to | to |
| via | through, with |
| launch, spin up, kick off | start |
| tear down | close, stop |
| detect (a lost peer) | find |
| e.g., i.e. | for example, that is |
| approx., ~ | approximately |
| etc. | and so on (or list all items) |
| simply, just, easily | (omit) |
| retry | try again |
| refer to | see |
| require | need |
| perform | do |
| sufficient | enough |

Fixed meanings in this project:

| Word | Meaning |
|---|---|
| open | Start a session or a connection. |
| close | Stop a session or a connection. |
| start | Begin to run the adapter or a task. |
| refuse | Answer a request with an error status. |
| drop | Discard a packet without a reply. |
| block | Wait in a call until it is complete or times out. |

## 3. Procedures

A procedure is a numbered list of steps that a reader does in sequence.

1. Give the procedure a title that starts with a verb: "Open a session and
   read a tag."
2. Put one instruction in each step.
3. Start each step with a verb in the imperative: "Set," "Call," "Make sure."
4. Put a condition first: "If the open fails, wait and do step 3 again."
5. Put a warning or a caution before the step that it applies to, not after.
6. Put a code listing after the step that tells the reader to use it.
7. End with a check, if one is possible: "Make sure that all tests pass."

Do not put a description inside a step. Put it before or after the
procedure.

## 4. Safety notices

Use three notice types. Each notice starts with a command or a statement of
the hazard, and it tells the reader how to prevent it.

| Notice | Use for |
|---|---|
| **WARNING** | A condition that can cause injury or death, or that can make machinery move when nobody expects it. |
| **CAUTION** | A condition that can cause damage to equipment, loss of data, or corruption of memory. |
| **NOTE** | Information that helps the reader. Not a hazard. |

QuickEIP controls industrial equipment. Add a warning in these cases:

- The text describes the safe state of consumed data (all zeros).
- The text describes a remote reset, an IP change, or another change that a
  scanner can make while machinery runs.
- The text describes a platform that is not real time, such as Windows.
- The text could make a reader treat QuickEIP as a safety function.
  QuickEIP does not implement CIP Safety.

Add a caution for mistakes that corrupt memory or break a build silently,
for example different `eip_config.h` values in C and C++ files.

## 5. Descriptions

- Start with what the item is or does. Then give the rules and limits.
- Tell why when the reason helps the reader obey a rule:
  "Declare it `static`, because it is too large for most task stacks."
- Give a list of rules as bullets after an introductory sentence:
  "These rules apply:"
- Use "you" for the reader and "your application" for the reader's code.

## 6. Titles and headings

Chicago headline style applies to the titles of the manual, chapters,
sections, tables, figures, and procedures in LaTeX:

- Capitalize the first and last words and all major words.
- Use lowercase for articles, coordinating conjunctions, and prepositions,
  unless they are first or last: "Connecting a PLC to the Adapter."
- Capitalize both parts of a hyphenated major word: "Build-Time
  Configuration."

The Markdown files in `docs/` use sentence-style headings with section
numbers ("## 4. Using the scanner"). Keep the style of the file that you
change. Use headline style for a new LaTeX chapter or section.

## 7. Punctuation

- **Serial comma.** Use it in lists of three or more: "TCP 44818, UDP
  44818, and UDP 2222."
- **En dash** for ranges: UDP 2223–2231, attributes 1–8, slot 0–255. In
  LaTeX, write `--`.
- **Em dash** without spaces. Prefer a period or a colon in STE text.
- **Quotation marks.** Put a period or a comma inside the closing mark:
  the header says "idle."
- **Colon** before a list, an example, or a code listing that completes the
  sentence.
- **Abbreviations.** Spell out an abbreviation at its first use in each
  document, unless it is in the glossary: "requested packet interval (RPI)."
  Do not use periods in abbreviations: PLC, CIP, RPI.

## 8. Numbers and units

Use the alternative rule of Chicago 9.3:

- Use words for zero through nine in running text: "three file groups,"
  "two ticks."
- Use numerals for 10 and above: "19 functions."
- Use numerals for all quantities with a unit, data values, instance
  numbers, port numbers, status codes, and code: "5 ms," "8 bytes,"
  "instance 3," "slot 0."
- Put a space between a numeral and its unit: 50 ms, 1.5 KB, 100 Mbit/s.
  In LaTeX, use a non-breaking space: `50~ms`.
- Use commas in numbers of five or more digits in text: 10,752 bytes.
  Do not use commas in code values or port numbers: 44818, 65000.
- Write hexadecimal values in code form with `0x`: `0x0127`. Write a CIP
  error as general / extended status: `0x01` / `0x0127`.
- Give timeouts in milliseconds and RPIs in microseconds, as the API does.

## 9. Tables, figures, and equations

- Refer to each table and figure in the text before it appears:
  "Table 4.3 gives the rules."
- **Captions** (LaTeX). Number them by chapter. Put the caption above a
  table and below a figure. Chicago form: label in bold, a period, then the
  caption text, for example **Table 4.3.** Forward_Open rules. Use sentence
  capitalization in caption text.
- **Table columns.** Make the first column the item that the reader looks
  up. Make the last column the explanation.
- **Equations.** Use an equation when a value comes from a calculation, for
  example a timeout or a buffer size. Define each symbol in a sentence before
  the equation, and give a worked example with the default values after it.

## 10. Notes and bibliography

Use the Chicago notes-bibliography system.

- Use a footnote to cite the source of a protocol rule, for example a
  section of the CIP specification. Do not use a footnote for an aside.
- First note, full form:
  `ODVA, The CIP Networks Library, vol. 2, EtherNet/IP Adaptation of CIP, sec. 3-5.3.`
- Bibliography entries: alphabetical by author, with a hanging indent.
  Repeat the same author with a 3-em dash (`---------` in LaTeX).

```
ODVA. The CIP Networks Library. Vol. 1, Common Industrial Protocol (CIP).
    Ann Arbor, MI: ODVA.
Sweeney, Adam G. QuickEIP: A Small EtherNet/IP Stack for Embedded Targets.
    Version 2.0.0. Source code repository, MIT License, 2026.
```

Italicize the titles of books and specifications. Put the titles of files
and chapters in quotation marks.

## 11. QuickEIP terms

Use these terms the same way in all documents:

| Term | Use |
|---|---|
| scanner (originator) | QuickEIP when it sends requests or opens connections. |
| adapter (target) | QuickEIP when a PLC connects to it. |
| consumed assembly | Data that the PLC writes (O→T). |
| produced assembly | Data that the PLC reads (T→O). |
| O→T, T→O | The two directions of a Class 1 connection. In Markdown, use the arrow character. In LaTeX, use `\OT` and `\TO`. |
| assembly size | The size that a user enters in an EDS file or a PLC. |
| wire size | Assembly size plus the 2-byte sequence count and, on O→T, the 4-byte run/idle header. |
| session | A registered TCP connection on port 44818. |
| port | The platform code that implements `eip_hal.h`. Not a network port. Write "TCP port" or "UDP port" for those. |
| poll mode, task mode | How the adapter runs on a platform. |

Use the vendor ID 65000 only as a marked placeholder. Do not invent an IP
address, a tag name, or a slot for a real installation. Use the example
values that the existing documents use.

## 12. Test status

Say exactly what has been done. Use these terms:

| Term | Meaning |
|---|---|
| Builds | Compiles and links. Nothing more. |
| Runs | Has run on the named hardware. |
| Tested with *PLC* | Has exchanged data with that PLC. |
| Not yet tested | Nobody has tried it. |

Do not write that a port "works" or "is supported" without one of these
terms. A successful build says nothing about the network.

## 13. Markdown and LaTeX forms

| Element | Markdown (`docs/*.md`, `README.md`) | LaTeX (`docs/manual/`) |
|---|---|---|
| Code word | `` `eip_adapter_start` `` | `\code{eip_adapter_start}` |
| File | `` `include/eip.h` `` | `\file{include/eip.h}` |
| UI element | *Project Properties* | `\menu{Project Properties}` |
| Term at first use | *originator* | `\term{originator}` |
| CIP status | `` `0x01` / `0x0127` `` | `\hex{01} / \hex{0127}` |
| Warning | `> **WARNING:** ...` | `\begin{warning} ... \end{warning}` |
| Caution | `> **CAUTION:** ...` | `\begin{caution} ... \end{caution}` |
| Note | `> **Note:** ...` | `\begin{note}\notelabel ... \end{note}` |
| Procedure | `**Procedure: Title**` and a numbered list | `\begin{procedure}{Title}` with `enumerate` |
| API entry | Header comment in `include/` | `\api{name}{prototype}`, then text |
| Range | `2223–2231` | `2223--2231` |

LaTeX notes:

- Write `.tex` files with an editor tool, not a shell heredoc. Some shells
  change `\\` to `\`.
- The `underscore` package loads last in `preamble.tex`, so `_` prints in
  text. Do not put an underscore in a `\label` key. Use `\api`, which makes
  a safe key.
- Build with `tectonic quickeip-manual.tex` in `docs/manual/`. Fix all
  errors and all overfull lines before you commit the PDF.

## 14. Checklist

Before you submit a documentation change, make sure that:

- [ ] Each procedure step has one instruction and starts with a verb.
- [ ] No sentence is longer than 20 words (procedure) or 25 words (description).
- [ ] The text has no contractions and no passive voice where an active form exists.
- [ ] Each word in the table in section 2 has its replacement.
- [ ] Warnings and cautions come before the step that they apply to.
- [ ] Lists of three or more items use the serial comma.
- [ ] Ranges use an en dash, and numbers follow section 8.
- [ ] Each table and figure is referred to in the text.
- [ ] Test status uses the terms in section 12.
- [ ] Facts agree with the headers in `include/`. If they do not agree, the header is correct.
