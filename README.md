# tree-sitter-webfluent

[Tree-sitter](https://tree-sitter.github.io/) grammars for
[WebFluent](https://github.com/monzeromer-lab/WebFluent), the web-first
language that compiles to HTML, CSS, JavaScript and PDF.

| Grammar | Files | Folder |
|---|---|---|
| `webfluent` | `.wf` (blocks in braces) | [`webfluent/`](webfluent) |
| `webfluentx` | `.wfx` (blocks by indentation) | [`webfluentx/`](webfluentx) |

There is one grammar, written once in `webfluent/grammar.js`.
`webfluentx/grammar.js` generates it again with the layout tokens on, and
its external scanner includes the braced one's. Change `webfluent/` and
regenerate both.

## Working on it

```bash
npm ci
npm run generate   # both parsers; commit the generated src/ with grammar.js
npm test           # both corpora (test/corpus, and the guide's blocks in test/guide)
```

The generated `src/parser.c` files are committed, so an editor can build
the parser without the tree-sitter CLI.

## Who uses it

- [webfluent-zed](https://github.com/monzeromer-lab/webfluent-zed), the Zed
  extension, pins a commit of this repository in its `extension.toml`.
- The WebFluent compiler repository links it as a submodule under
  `editors/tree-sitter-webfluent`.

## Licence

MPL-2.0, as the compiler.
