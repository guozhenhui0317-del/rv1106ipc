# EasyLogger core

This directory contains only the EasyLogger files required by `gzh_ipc`:

- `include/elog.h`
- `src/elog.c`
- `src/elog_utils.c`

The source was copied from [armink/EasyLogger](https://github.com/armink/EasyLogger) at commit
`806328e131836662fa83dd43364a53f699fd76ac`. The upstream MIT license is retained in `LICENSE`.

Project-specific configuration and the Linux output port remain in `include/elog_cfg.h` and
`src/elog_port.c`, respectively. Upstream demos, plugins, documentation-site files and tests are
not needed by this application and are intentionally omitted.
