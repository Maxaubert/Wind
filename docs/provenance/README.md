# Provenance records

Each `YYYY-MM-DD-<commit>.txt` names a commit of Wind, its tree and the SHA-256 of
`git archive --format=tar <commit>`. The matching `.ots` file is an
[OpenTimestamps](https://opentimestamps.org) proof that the record, and so that commit and every
commit before it (git hashes chain), existed no later than the time it was anchored in the Bitcoin
blockchain. Nobody, including the owner, can backdate or alter such a proof.

Release builds attach the same kind of record and proof to each GitHub release
(`provenance-<version>.txt` and `.ots`).

## Verify

```
pip install opentimestamps-client
ots upgrade 2026-10-27-8566da3.txt.ots   # once, after a few hours: fetches the Bitcoin attestation
ots verify 2026-10-27-8566da3.txt.ots    # needs a Bitcoin node, or check the result at opentimestamps.org
git rev-parse 8566da3                     # the commit named in the record
```

A fresh proof is pending until a calendar server anchors it (usually within a few hours). Run
`ots upgrade` on it and commit the upgraded file so the proof stands alone.

On Windows the `ots` command can fail to load OpenSSL through python-bitcoinlib; running it on
Linux or WSL avoids that.
