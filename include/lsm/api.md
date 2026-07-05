# Engine API contract (Section 0.5)

Plain-English behavior of the four public methods. Code in later sections must
match this document; if they disagree, this document wins.

## Put(key, value)

- If `key` already exists, the new value replaces the old one.
- An **empty key is illegal** (`InvalidArgument`); an **empty value is allowed**.
- On success, a later `Get(key)` returns this value.
- A write is only acknowledged after it is durable per the WAL sync policy
  (Section 1).

## Get(key)

- Returns the latest visible value for `key`.
- If the latest record for the key is a tombstone (from `Delete`), the key is
  treated as **not found**.
- An empty key is illegal (`InvalidArgument`).

## Delete(key)

- Marks the key as deleted by writing a tombstone; older values are hidden.
- Deleting a non-existent key is **not an error** (a DEL record is still
  logged).
- An empty key is illegal (`InvalidArgument`).

## Close()

- Finishes all outstanding work (flushes and syncs the WAL) and releases
  resources.
- After `Close`, any call returns a `StoreClosed` error.

## Error names

Standardized in [errors.md](errors.md): `InvalidArgument`, `StoreClosed`,
`IOFailure`, `CorruptionDetected`, `NotImplemented`.
