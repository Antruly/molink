# Server data files

The mainland-China IP table is generated from the public APNIC delegated
statistics and embedded into `molink-server` at build time:

```sh
python3 -I server/tools/gen-cn-cidr.py          # refresh the table
python3 -I server/tools/gen-cn-cidr.py --check  # detect drift (CI or manual)
```

The generated file is `server/src/security/cn_cidr_generated.inc`. The current
checked-in copy is a small placeholder so the classifier and its unit tests
work offline; run the generator once on a networked machine to replace it with
the full snapshot. The generator records the snapshot date and the SHA-256 of
the source file in the header.

Only IPv4 allocations are embedded. IPv6 mainland-China ranges are not part of
the v1 policy: public IPv6 clients are treated as overseas and must be enabled
explicitly (or reach the server through a trusted proxy with an IPv4 address).

Attribution: APNIC delegated statistics, <https://ftp.apnic.net/stats/apnic/>.
