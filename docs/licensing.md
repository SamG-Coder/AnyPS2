# Licensing and distribution

AnyPS2's original source and documentation are released under the [MIT license](../LICENSE).
MIT permits use, modification and redistribution, including commercial use, with
retention of its copyright and permission notice. It does not require forks to
publish their changes. See the [OSI license text](https://opensource.org/license/mit).

## Basis for the choice

The recorded provenance identifies AnyPS2's implementation and synthetic fixtures
as original work. The current build downloads zlib and HIDAPI separately; their
permissive terms can be retained alongside MIT. Consulting an emulator's documented
interfaces is different from incorporating its implementation. The project records
PCSX2 and God of War recompilation as references, not source donors or linked
runtime components. MIT is not permission to relicense any future GPL-derived code.
Any such contribution needs a fresh license review before integration.

This assessment is based on the source, build definitions, retained notices and
provenance records reviewed for this release. It is not a legal opinion or a proof
that every historical line is independently authored.

| Component | Use and license treatment |
| --- | --- |
| AnyPS2 original source and synthetic tests | MIT; retain the root license |
| zlib 1.3.1 | Downloaded importer dependency; retain the [zlib notice](third-party/zlib-LICENSE.txt) and identify modifications |
| HIDAPI at `dbff4ea89f55a572aeb0c53a7b32ea70853ec260` | Downloaded Windows USB dependency; AnyPS2 selects its BSD option. Retain the [BSD notice](third-party/hidapi-license.txt) with binary distributions |
| Play! | Public behavioral reference; retained [BSD notice](third-party/Play-license.txt), not relicensed as AnyPS2 |
| LLVM/Clang, Visual Studio, Vulkan SDK and Windows APIs | External build tools/platform dependencies with their own terms; this source release does not redistribute them |
| PS2DEV, PS2SDK and PCSX2 | Optional development/reference tools with their own licenses; not bundled into this source release or used as a native instruction fallback |
| glampert cube demo | Separately acquired homebrew integration input; upstream source is MIT, while bundled assets require separate review. No demo source, assets, input ELF or converted binary is distributed here |

Pinned versions, uses and reference files are recorded in
[source references](source-references.md). Evaluated library candidates in that
ledger are not necessarily build dependencies. Do not infer linked dependencies
from a mention in the documentation.

## Game content is outside this license

MIT covers the project, not a user's game, BIOS, firmware, SDK, assets, trademarks,
or converted game executable. No rights to those materials are granted. Do not
publish them as source attachments, test fixtures, release assets or issue logs.
Game names identify compatibility targets and do not imply affiliation.
Read the [usage disclaimer](../DISCLAIMER.md).

A binary release needs its own dependency and notice review. Distribute native
service/tool binaries only after that review; exclude converted commercial games
and locally downloaded reference tools. Do not upload a working-directory ZIP.
