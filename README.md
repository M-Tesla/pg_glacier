# pg_glacier

[Glacier](https://github.com/M-Tesla/Glacier) extension for PostgreSQL.

It runs in PostgreSQL. Add it to `shared_preload_libraries`, restart, and `CREATE EXTENSION pg_glacier`.

## Build

`pg_config` on `PATH`, plus `libglacier.so`. `GLACIER_INCLUDE` and `GLACIER_LIBDIR` default to `../Glacier/include` and `../Glacier/zig-out/lib`.

```
make
```

Without `pg_config`, set `PG_SERVER_INCLUDE` to the server headers.
