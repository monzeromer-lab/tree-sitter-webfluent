/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// The grammar of a `.wfx` file: WebFluent 4 with its blocks written by
// indentation. It is `../webfluent/grammar.js` generated with
// the layout tokens on — one grammar file, two parsers — so this file only
// sets the flag and hands over. Its scanner (`src/scanner.c`) includes the
// base grammar's, under this grammar's name.
process.env.WFX = "1";
module.exports = require("../webfluent/grammar.js");
