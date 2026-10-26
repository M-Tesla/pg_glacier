# pg_glacier

[Glacier](https://github.com/M-Tesla/Glacier) extension for PostgreSQL. This extension is 0.6.3. It links `libglacier.so` from Glacier 0.7.0, `api_version` 2.

It runs in PostgreSQL. Add it to `shared_preload_libraries`, restart, and `CREATE EXTENSION pg_glacier`.

`glacier_query(path, sql)` runs one Glacier statement and returns rows. A foreign table does the same for an ordinary `SELECT`, including a join with a local table:

```
CREATE SERVER glacier FOREIGN DATA WRAPPER glacier_wrapper;
CREATE FOREIGN TABLE orders (id bigint, note text)
  SERVER glacier OPTIONS (path '/warehouse/sales/orders');
SELECT COUNT(*) FROM orders o JOIN local_customer c ON c.id = o.id;
```

`path` is a Parquet file, an Iceberg table, or a warehouse. On a warehouse, set `relation` to the Glacier name, such as `sales.orders`. A filter PostgreSQL can ship is sent in the Glacier statement. The rest is applied by PostgreSQL. `SUM` on the foreign table is computed by PostgreSQL unless you put it in `glacier_query`.

## Build

`pg_config` on `PATH`, plus `libglacier.so`. `GLACIER_INCLUDE` and `GLACIER_LIBDIR` default to `../Glacier/include` and `../Glacier/zig-out/lib`.

```
make
```

Without `pg_config`, set `PG_SERVER_INCLUDE` to the server headers.
