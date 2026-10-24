# PostgreSQL client headers (build-time only)

`libpq-fe.h`, `postgres_ext.h` and `pg_config_ext.h` come from PostgreSQL
16.15 (`libpq-dev` 16.15-0ubuntu0.24.04.1). They are vendored so Windows CI can
compile libuvcpp's PostgreSQL backend without installing a PostgreSQL server.

The server loads `libpq` at runtime (`UVCPP_DB_RUNTIME_LOAD=ON`), so these
headers are the only build-time dependency; the target machine needs the
`libpq` client library only when `database.driver` is `postgres`.

Linux uses the distribution's `libpq-dev`, macOS uses `brew install libpq`.
Upgrade this directory together with the libuvcpp PostgreSQL runtime-load
contract; see `doc/db-driver-policy.md` in libuvcpp.

License: PostgreSQL License (see `LICENSE`).
