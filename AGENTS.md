# Repository Guidelines

- Follow the existing C coding style when editing files in `src/`: 4-space indents, no tabs, braces on the same line as the control statement, and keep lines under 120 characters when practical.
- Keep the documentation (`README.md`, `configs/`, `handler_contract.txt`) in sync with any changes to build steps, configuration keys, or HTTP/CLI interfaces you introduce.
- Prefer updating or adding unit tests or sample configuration snippets when you change execution or networking behavior.
- Use the top-level `Makefile` targets instead of ad-hoc compiler invocations so build instructions stay accurate.
- UI screenshots are not required unless explicitly requested.
