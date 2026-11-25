# AI agents

Two ways let an AI agent convert PDFs with RayoPDF.

## Agent Skill

[`skills/rayopdf`](../../skills/rayopdf/SKILL.md) is an Agent Skill (a `SKILL.md` file with
instructions an agent loads when a task matches). Copy the folder into the agent's skills folder
(for Claude Code: `~/.claude/skills/rayopdf`). It tells
the agent how to get and verify the binary, convert a PDF or a folder of them, choose the
options, check the `--report` record and read the JSON output.

## MCP server

[rayopdf-mcp](https://github.com/Butterski/rayopdf-mcp) is a
[Model Context Protocol](https://modelcontextprotocol.io/) server around one long-running
`rayopdf --serve --json` process. Its tools `pdf_to_markdown` and `pdf_to_json` convert the PDFs
under the folders it is given with `--root`. Its README explains how to install and configure
it.

## Your own tool

A tool of your own can keep one `rayopdf --serve --json` process running and send it one JSON
request a line ([command-line guide](cli.md#a-long-running-process---serve---json)), or call
the converter in-process from [Python](python.md), [JavaScript](javascript.md) or
[C and C++](cpp.md). For retrieval-augmented generation, `--format=chunks` cuts the document
into chunks with their heading path and pages ([output formats](formats.md#chunks---formatchunks)).
