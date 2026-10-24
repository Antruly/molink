# Third-party notices

- **APNIC delegated statistics** — `server/src/security/cn_cidr_generated.inc` is
  generated from <https://ftp.apnic.net/stats/apnic/>. The snapshot date and
  source SHA-256 are recorded in the file; regenerate with
  `server/tools/gen-cn-cidr.py`.
- **PostgreSQL client headers** — `server/third_party/postgresql/` contains
  `libpq-fe.h`, `postgres_ext.h` and `pg_config_ext.h` from PostgreSQL 16.15,
  under the PostgreSQL License (see the LICENSE file in that directory). They
  are used only to compile libuvcpp's PostgreSQL backend; the client library is
  loaded at run time.
- **libuvcpp / libuv / llhttp / zlib / SQLite / OpenSSL** — build and runtime
  dependencies of the server and desktop client; their licenses are included in
  the respective source trees and packages.
