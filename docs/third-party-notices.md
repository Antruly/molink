# Third-party notices

- **APNIC delegated statistics** — `src/security/cn_cidr_generated.inc` in the
  [moLink Server](https://github.com/Antruly/molink-server) repository is
  generated from <https://ftp.apnic.net/stats/apnic/>. The snapshot date and
  source SHA-256 are recorded in the file; regenerate with
  `tools/gen-cn-cidr.py`.
- **PostgreSQL client headers** — `third_party/postgresql/` in the moLink Server
  repository contains `libpq-fe.h`, `postgres_ext.h` and `pg_config_ext.h` from
  PostgreSQL 16.15, under the PostgreSQL License (see the LICENSE file in that
  directory). They are used only to compile libuvcpp's PostgreSQL backend; the
  client library is loaded at run time.
- **libuvcpp / libuv / llhttp / zlib / SQLite / OpenSSL** — build and runtime
  dependencies of the server and desktop client; their licenses are included in
  the respective source trees and packages.
- **SVT-AV1** (`libSvtAv1Enc`) — optional AV1 rectangle encoder. The library is
  BSD-2-Clause plus the Alliance for Open Media Patent License 1.0; it is only
  linked when the build enables `MOLINK_ENABLE_AV1`, and it is not part of the
  default packages yet.
- **dav1d** (`libdav1d`) — optional AV1 decoder, BSD-2-Clause. Used only by
  `MOLINK_ENABLE_AV1` builds to decode AV1 rectangles on the client core.
