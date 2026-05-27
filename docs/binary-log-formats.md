# Binary Log Formats: Prior Art for the Mailbox

Survey of binary append-only log formats from the broader ecosystem, framed against our current mailbox layout. The goal is to map the design space, not declare a winner.

## Our format, in one paragraph

One file per recipient at `<state>/mailbox/<name>.log`. A 64-byte header carries `magic "BUS\0"`, `version u32 le`, `cursor u64 le`, `created_ms u64 le`, and 40 reserved bytes. Records append after the header as `record_len u64 le | sent_ms u64 le | rand u16 le | type u8 | priority u8 | sender_len u8 | sender utf-8 | body_len u32 le | body utf-8`. Atomicity comes from `O_APPEND` when records fit under `PIPE_BUF` (4096 bytes). A single consumer per mailbox advances `cursor` with `pwrite` at offset 8. No segmentation, no checksums, no cleanup.

## Our format vs the field

| Concern | Us | Common in field |
| --- | --- | --- |
| Header magic + version | Yes (4+4 bytes) | Yes — almost universal (Avro `Obj\x01`, LevelDB SST footer magic, Arrow `ARROW1`, MySQL `0xFE62696E`) |
| Per-record checksum | **No** | Yes — Kafka (CRC-32C), TFRecord (two CRC-32Cs), LevelDB WAL (CRC-32C), PostgreSQL WAL (CRC-32C), MySQL binlog (CRC32) |
| Per-record framing | Length-prefix (`record_len u64`) | Length-prefix dominant; LevelDB WAL uses 32 KiB blocks with FIRST/MIDDLE/LAST fragments |
| Sync/recovery marker | **No** | Avro embeds a 16-byte random marker per block; others rely on length+CRC |
| Segmentation | Single growing file | Kafka rolls segments (`<base-offset>.log/.index/.timeindex`); BookKeeper splits entry logs |
| Sidecar index | **No** | Kafka `.index`/`.timeindex`; Arrow file-mode footer; LevelDB SST footer; BookKeeper ledger index |
| Read cursor | In-header `pwrite` at offset 8 | Usually external (Kafka consumer offsets in a separate topic, etc.) |
| Endianness | Little-endian | Mixed — most modern formats little-endian; PostgreSQL WAL is host-endian; Kafka wire protocol is big-endian |

The shape we have — header + length-prefixed records — is the mainstream baseline. The notable absences from our design are checksums, sync markers, and segmentation. The notable addition is the in-header consumer cursor.

## Prior art

### RecordIO (Google or-tools, MXNet/dmlc, SageMaker)

RecordIO is the lowest-common-denominator: a stream of length-prefixed binary chunks, typically each chunk a serialized protobuf. There is no formal spec; implementations vary on whether they prefix a stream-level magic, on the length width (4 vs 8 bytes), and on whether records carry their own type tag. The dmlc variant adds a 4-byte magic per record (`0xCED7230A`) plus a 4-byte length with the low 2 bits encoding flags (continue/start/end/full) — strikingly similar to LevelDB WAL's fragment-type scheme.

- [Apache Mesos RecordIO](https://mesos.apache.org/documentation/latest/recordio/)
- [dmlc-core recordio.h](https://dmlc-core.readthedocs.io/en/latest/doxygen/recordio_8h.html)

### TFRecord (TensorFlow)

Each record is twelve bytes of header plus payload plus a four-byte trailer: `uint64 length | uint32 masked_crc32c_of_length | byte data[length] | uint32 masked_crc32c_of_data`. CRCs are CRC-32C with a masking transform `((crc >> 15) | (crc << 17)) + 0xa282ead8` so that an in-payload CRC can't be mistaken for a record header. There is no file-level header; the file is a pure sequence of records. Strictly sequential read.

- [TFRecord format reference](https://www.tensorflow.org/tutorials/load_data/tfrecord)
- [Anatomy of TFRecord (Jong Wook Kim)](https://jongwook.kim/blog/Anatomy-of-TFRecord.html)

### LevelDB WAL (log_format.md)

The most thoughtful design in this list. The file is a sequence of 32 KiB *blocks*; each block is a sequence of records, each record is `checksum u32 | length u16 | type u8 | data[length]`. When a record doesn't fit in the current block's remainder, it's split into FIRST/MIDDLE/LAST fragments — the reader rebuilds the logical record. A record never starts within the last 6 bytes of a block (because the header wouldn't fit), so those bytes are zero-padded. This block discipline lets a corrupt region be skipped to the next block boundary without losing the rest of the log. CRC is CRC-32C over type+data, little-endian.

- [LevelDB log_format.md](https://github.com/google/leveldb/blob/main/doc/log_format.md)

### LevelDB SSTable (table_format.md)

Different beast — read-optimized, immutable. Data blocks → meta blocks → metaindex block → index block → 48-byte footer at `file_size - 48`. Footer carries the metaindex/index block handles plus a fixed magic `0xdb4775248b80fb57`. The fixed-size *trailer* convention (rather than fixed-size header) lets a reader seek directly to file end and bootstrap everything else. Not append-only; built by full rewrite during compaction.

- [LevelDB table_format.md](https://github.com/google/leveldb/blob/main/doc/table_format.md)

### Kafka log segments (v2 record batch)

Each partition is a directory of triplets: `<base-offset>.log` (data), `<base-offset>.index` (offset → file position), `<base-offset>.timeindex` (timestamp → relative offset). Index entries are fixed 8 bytes for the offset index and 12 bytes for the time index; both are *sparse* — entries every few KiB, not per record. The data file is a sequence of v2 RecordBatch structures with their own header: `FirstOffset i64 | Length i32 | PartitionLeaderEpoch i32 | Magic i8 | CRC i32 (CRC-32C) | Attributes i16 | …timestamps, ProducerId for EOS…`. CRC starts *after* the partition leader epoch so the broker can stamp the epoch without recomputing. Segments roll on size or time.

- [Kafka message format v2](https://kafka.apache.org/22/implementation/message-format/)
- [Strimzi: segments, rolling, retention](https://strimzi.io/blog/2021/12/17/kafka-segment-retention/)

### Apache Avro Object Container File (OCF)

File header is `'O','b','j',0x01 | metadata-map (schema as JSON in `avro.schema`, codec in `avro.codec`) | sync_marker[16]`. The file is then a sequence of blocks of `object_count varint | block_byte_size varint | (codec-compressed objects) | sync_marker[16]`. The repeated 16-byte random marker is the standout pattern: a reader who lands mid-file (e.g. after a corrupted block) can scan forward for the marker and resume at the next block boundary. The 16-byte width makes random collision astronomically unlikely.

- [Avro 1.11 specification](https://avro.apache.org/docs/1.11.1/specification/)

### Apache Arrow IPC (file mode / Feather v2)

`ARROW1\0\0 | streaming-format-body | FOOTER (flatbuffer) | footer_size i32 | ARROW1\0\0`. The footer is a flatbuffer holding the schema (redundant with the streaming body) and an array of `Block{offset i64, metaDataLength i32, bodyLength i64}` entries. Footer-at-end means random access without scanning. Magic at both ends defends against truncation.

- [Arrow IPC format](https://arrow.apache.org/docs/format/Columnar.html#ipc-file-format)

### PostgreSQL WAL

WAL is a sequence of 16 MiB segment files. Each XLog record has a header carrying total length, transaction id, the previous-record pointer, a resource manager id, and a CRC-32C over the entire record. The back-pointer lets a reader scan backward; the CRC is checked during replay and replication. Records can span pages, and each WAL page itself has a small page header (with its own checksum derivation).

- [PostgreSQL WAL reliability](https://www.postgresql.org/docs/current/wal-reliability.html)
- [WAL deep dive (Postgres Pro)](https://postgrespro.com/blog/pgsql/5967958)

### MySQL binlog

File starts with a magic (`0xFE62696E` plain, `0xFD62696E` encrypted — "bin" in ASCII with a sentinel byte). Each event has a fixed 19-byte common header (timestamp, type, server id, total size, log position, flags) followed by event-specific payload. When `binlog_checksum=CRC32` (default since 5.6.6), every event carries a trailing 4-byte CRC32. The first event in every file is a `Format_description_event` that bootstraps the reader's interpretation — effectively a self-describing schema record.

- [MySQL 8.4: The Binary Log](https://dev.mysql.com/doc/refman/8.4/en/binary-log.html)
- [Format_description_event reference](https://dev.mysql.com/doc/dev/mysql-server/8.0.41/classbinary__log_1_1Format__description__event.html)

### Apache BookKeeper

A *ledger* is the append-only abstraction; on a bookie, entries from many ledgers are interleaved into shared *entry log files* with a per-ledger sidecar index pointing into them. Ledger metadata is protobuf-encoded and stored separately (ZooKeeper / etcd / table service). The split — many logical streams onto few physical files, plus a per-stream index — is the inverse of our "one file per recipient" choice and shows up whenever there are many low-volume streams.

- [BookKeeper concepts](https://bookkeeper.apache.org/docs/4.5.1/getting-started/concepts/)

### Length-delimited protobuf (gRPC, `writeDelimitedTo`)

Each message is preceded by a varint of its size, then the protobuf body. Trivial and ubiquitous. There is no file header, no checksum, no schema embedding — the format relies entirely on the proto schema being known out-of-band and the transport (TCP, file) being reliable. This is roughly what RecordIO degenerates to without a magic.

- [Protobuf encoding: length-delimited](https://protobuf.dev/programming-guides/encoding/)
- [Apache Geode: Delimiting Protobuf Messages](https://cwiki.apache.org/confluence/display/GEODE/Delimiting+Protobuf+Messages)

## Patterns we should consider adopting

1. **Per-record CRC-32C.** Universal: TFRecord, Kafka v2, LevelDB WAL, PostgreSQL WAL, MySQL binlog. A 4-byte trailer (or leading field, after the length) over `sent_ms..body` catches truncated appends, partial writes from a crash mid-record, and bit-rot. CRC-32C has a fast hardware path (`_mm_crc32_*` / ARMv8 `crc32cb`) so it's not even an excuse on perf grounds. Cheapest single upgrade to robustness.

2. **TFRecord's two-CRC trick (length + payload).** Storing a CRC of *just the length field* in front of the payload means a corrupted length can't trick the reader into reading gigabytes of garbage. Useful for us because `record_len` is `u64` — a flipped high bit lands us in absurd territory. Cheap insurance even if we don't checksum the body.

3. **Magic in each record, not just file header.** dmlc-recordio and Kafka batches both do this. With our `record_len u64` first, a bad length yields no recovery; a 2- or 4-byte magic at the start of each record means a corrupted region can be re-synced by scanning. The cost is 2-4 bytes per record.

4. **First-record self-description (MySQL `Format_description_event` style).** If we ever bump `version`, the reader needs a way to learn the layout without consulting external docs. Reserving room in the 40 spare header bytes for either inline schema or a pointer to a "format descriptor" first record costs nothing now and avoids version-skew pain later.

5. **Segment rolling (Kafka style).** Not urgent at our volumes, but the day a mailbox is left running for a week we'll want it. The natural shape: rename `name.log` to `name.<base-offset>.log` past a size or age threshold, start fresh. Single-consumer state stays in one place if `cursor` continues across segments.

## Patterns that are overkill for us

- **Sync markers (Avro).** They only earn their keep when blocks are large and random access matters. Our records are <4 KiB and consumed sequentially — a per-record magic gives the same resync ability for fewer bytes.
- **Sparse offset/time index sidecars (Kafka).** Justified only when you need to seek into millions of records by offset or timestamp. We have a single cursor; there is nothing to seek to.
- **Block discipline with fragmentation (LevelDB WAL).** Solves the case of records larger than the atomic-write unit. Our `O_APPEND` + `PIPE_BUF` cap already guarantees atomicity for records ≤ 4 KiB; we'd inherit fragmentation complexity to handle a case we've defined away.
- **Footer-at-end + flatbuffer schema (Arrow).** Optimised for read-once over an immutable file. Our file is being written continuously; a trailer that has to be rewritten on every append is the wrong shape.
- **Schema registry / external metadata (BookKeeper, Schema Registry).** We have one schema, defined in C++. Premature.
- **Compression codecs (Avro `avro.codec`, Kafka batch compression).** Bodies are short text. Compression would cost more in code than it saves on disk.
- **Big-endian wire order (Kafka).** We're not interoperating with the JVM ecosystem; little-endian matches the host and avoids byte-swap on every field.

## Endianness note

Most modern formats settle on little-endian (LevelDB, TFRecord, Avro varints aside). Kafka is the conspicuous big-endian outlier because of Java's `DataOutputStream` legacy. PostgreSQL WAL stays host-endian on the assumption that a WAL stream is never read on a different architecture than it was written on — a choice that complicates pg_basebackup across arches. Our little-endian choice is the safe default.

## Sources

- [Apache Mesos: RecordIO Data Format](https://mesos.apache.org/documentation/latest/recordio/)
- [dmlc-core recordio.h reference](https://dmlc-core.readthedocs.io/en/latest/doxygen/recordio_8h.html)
- [TensorFlow TFRecord format](https://www.tensorflow.org/tutorials/load_data/tfrecord)
- [Anatomy of TFRecord](https://jongwook.kim/blog/Anatomy-of-TFRecord.html)
- [LevelDB log_format.md](https://github.com/google/leveldb/blob/main/doc/log_format.md)
- [LevelDB table_format.md](https://github.com/google/leveldb/blob/main/doc/table_format.md)
- [Kafka message format v2](https://kafka.apache.org/22/implementation/message-format/)
- [Strimzi: Kafka segments, rolling, retention](https://strimzi.io/blog/2021/12/17/kafka-segment-retention/)
- [Apache Avro 1.11 specification](https://avro.apache.org/docs/1.11.1/specification/)
- [Apache Arrow IPC format](https://arrow.apache.org/docs/format/Columnar.html#ipc-file-format)
- [PostgreSQL WAL reliability](https://www.postgresql.org/docs/current/wal-reliability.html)
- [Postgres Pro: WAL deep dive](https://postgrespro.com/blog/pgsql/5967958)
- [MySQL 8.4: The Binary Log](https://dev.mysql.com/doc/refman/8.4/en/binary-log.html)
- [MySQL Format_description_event](https://dev.mysql.com/doc/dev/mysql-server/8.0.41/classbinary__log_1_1Format__description__event.html)
- [Apache BookKeeper concepts](https://bookkeeper.apache.org/docs/4.5.1/getting-started/concepts/)
- [Protobuf encoding (length-delimited)](https://protobuf.dev/programming-guides/encoding/)
- [Apache Geode: Delimiting Protobuf Messages](https://cwiki.apache.org/confluence/display/GEODE/Delimiting+Protobuf+Messages)
