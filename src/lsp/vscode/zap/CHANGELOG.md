# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-07-24

### Added

- Completion, definition, hover, and signature help backed by shared semantic snapshots.
- Imports are read from `thor.toml`; the extension uses the `zap-lsp` installed by `zapup`.
- UTF-16 position handling, multi-file diagnostics, document lifecycle support, and request cancellation.

### Changed

- The packaged extension bundles `zap-lsp` but uses the Zap installation's `core` and `std` directories.

## [0.0.1] - 2026-03-16

### Added
- Initial release with Zap LSP support and syntax highlighting.

### Changed

### Removed
