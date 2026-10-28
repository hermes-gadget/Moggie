# Stock maintenance code paths: offline evidence

2026-10-28. **Verified for the captured 57.10-25140 Fenway userspace; BLOCKED for exact ZP5S applicability.** The capture identifies itself as `ARCH fenway` (`R/etc/inittab:1`). The prior target identification is ZP5S/MPC8272 and explicitly warns against transferring the sibling build's userspace conclusions (`P/MODEL-IDENTITY-CAVEAT.md:12–47`). No hardware observation was repeated in this investigation.

The principal results are:

- Normal `/unlock` verifies a signature, attempts to start a fixed script, and creates temporary unlock state. Its normal response does **not** select the reboot call. See [U1–U5](#normal-unlock).
- The located `/unlock` route predicate requires a TLS connection; the previously reported `0x4000` loopback gate is a directory check. This contradicts that explanation in the prior recon for this captured build. See [N1–N5](#network-preconditions).
- `/devmode` accepts a verified binary statement, with persistence controlled by signed bit 0. `/testenv` separately changes persistent settings when its debug-property gate permits it. See [D1–D4](#devmode) and [T1–T3](#testenv).

## Evidence notation and limits

`R` = `/home/ben/sonos-bench/sonos-fenway/firmware/rootfs/57.10-25140`.
`P` = `/home/ben/.hermes/plans/2026-10-28-moggie-recon-output`.
The following image abbreviations always identify these exact captured files:

| ID | File relative to R | SHA-256 |
|---|---|---|
| A | `opt/bin/anacapad` | `2a0e6adb831d995a2863f4307077e2c14c59afc52a43815865f0bc09f8580962` |
| C | `lib/libsonoscrypto.so.1` | `f89d45c2a16a573d960d4afe5081e0d0ae67017844681bd7f1f22fdf053d9874` |
| U | `lib/libsonosutils.so.1` | `eaf3493555cb1c5b9aac6eed89b22209eb9288a020035158cdda052478e2af3c` |
| D | `usr/bin/dropbear` | `9be533325e83a604e3ee992a7b1b7388449a01bfd702f5d603b1dbee75a965b4` |

Hashes, sizes, ELF headers, load mappings, script hashes and symlink metadata are in [manifest.json](evidence/unlock-20261028/manifest.json). `A@0x2a0bb0` means **file byte offset**, never a virtual address. A code/rodata VA is its file offset plus `0x10000000`; A's second file-backed load segment instead uses `VA = file + 0x10010000`. These mappings come from A's ELF program headers at file `0x34`. C/U/D code offsets in the excerpts equal their link-time VAs; shared-library runtime load bases are unspecified.

Every disassembly line records both VA and file offset. Unnamed functions are identified by address; objdump's nearest-symbol labels such as `strlcpy@@Base+...` do not name those functions. A's runtime state at VA `0x105817d0`, key buffer at VA `0x10676b78`, and imported copy-relocation slot at VA `0x1057200c` are outside its file-backed bytes: **there is no corresponding file offset** (A program headers; [relocations.json](evidence/unlock-20261028/relocations.json), A relocation entry file `0x4f34`). No runtime key, nonce, account hash or credential is included.

Confidence below means static control-flow confidence for the identified bytes. None of the response codes, daemon starts, writes, persistence or rollback effects was exercised on a speaker or in target emulation.

## Applicability and version inventory

The bounded inventory consists of the five `*/opt/bin/anacapad` files under `R/..`, with exact strings and endian-aware pointer candidates recorded in [versions.json:1–381](evidence/unlock-20261028/versions.json). A candidate pointer alone establishes neither runtime registration nor equivalent behavior. The detailed analysis below applies only to A.

| Captured directory | ELF identity | Maintenance evidence: file offsets | What is established |
|---|---|---|---|
| `16.6-00002-diag` | ELF32 big-endian PPC | `/unlock` string `0x2fe610`; pointer `0x3b110c` followed by VA `0x1019fe18`, flags `0` | Older entry candidate; no transfer of 57.10 crypto/effects. |
| `18.0-50150` | ELF32 big-endian PPC | `/unlock` string `0x2fdde8`; pointer `0x3b0694` followed by VA `0x101a03fc`, flags `0` | Older entry candidate; no transfer of 57.10 behavior. |
| `28.1-83040` | ELF32 **little-endian SuperH** | `/unlock` string `0x2a65e4`, table candidate `0x34b80c` → VA `0x005ab1c0`; alias pointer `0x34b820` | Different ISA; not disassembled with the PPC toolchain. A second `/unlock` pointer at `0x21e84c` is only a candidate reference. |
| `34.16-37101` | ELF32 big-endian PPC | `/unlock` pointer `0x447b38` → VA `0x102866ac`; `/devunlock` `0x447b68` and `/mfgunlock` `0x447b80` → VA `0x102866a4`; `/devmode` `0x447b98` → VA `0x10287618`; `/testenv` `0x447bb0` → VA `0x101c5d54` | Entries differ from 57.10; unlock-family candidate flags are `1`, not `0x10`. Their policy semantics remain unclosed. |
| `57.10-25140` | ELF32 big-endian PPC | Exact routes and handlers below; A program headers `0x34`, ELF header machine field `0x12` | Full named-path analysis in this report. |

The exact file `etc/scripts/run_sshd.sh` is present only in the 57.10 directory among these five roots (`versions.json:run_sshd_exists`). Its absence elsewhere does not prove absence of inline daemon startup or shell access.

The separate captured package `/home/ben/sonos-bench-analysis/zp5s/57.23-74170-1-16.upd` has SHA-256 `e19a7bb28f2d940346c605ccfcff81f8d9a7dd1771ee991841ac1f9e07223ad4`. Its directory contains only that package, and a whole-file magic search finds no ELF, gzip, squashfs, cramfs or uImage signature ([target-package.json:2–17](evidence/unlock-20261028/target-package.json)). This is a bounded negative result, **not** proof of encryption or impossibility of extraction. It supplies no extracted matching handler bytes for this report. The target build mismatch therefore remains unresolved; supply already-captured matching ZP5S userspace and extraction provenance to close it without hardware work.

## Handler discovery

The static module root at A file `0x55f410` points to module VA `0x1056c9ac`; that module's `+0x34` word at file `0x55c9e0` points to exact-route table VA `0x1056cae4`. The `+0x38` word at file `0x55c9e4` points to a separate prefix table. Complete exact entries and the terminator at file `0x55d52c` are preserved in [routes.json](evidence/unlock-20261028/routes.json).

The exact-entry stride is **28 bytes**: runtime hash/list fields occupy `+0/+4/+8`, path is `+0x0c`, handler `+0x10`, flags `+0x14`, extra word `+0x18`. The initializer walks this layout (A@`0x430bd0–0x430c60`), and dispatch reads flags and invokes the handler (A@`0x430d04–0x430ddc`). Registration starts from the module root at A@`0x42968c–0x429698` through VA `0x1043064c` ([module root](evidence/unlock-20261028/http-module-root.dis.txt), [registration](evidence/unlock-20261028/http-module-registration.dis.txt), [dispatch](evidence/unlock-20261028/http-route-dispatch.dis.txt)). Table presence plus this registration path is stronger evidence than a string hit; it still is not a runtime trace.

| Interface | A route structure file offset | Path/handler/flags file offsets | Function VA; code file offset |
|---|---|---|---|
| `/unlock` | `0x55cf60` | `0x55cf6c / 0x55cf70 / 0x55cf74` (`0x10`) | `0x102a0bb0`; `0x2a0bb0`, passes mode 0 |
| `/unlock.htm` | `0x55cf7c` | `0x55cf88 / 0x55cf8c / 0x55cf90` (`0x10`) | Same normal-unlock entry |
| `/mfgunlock` | `0x55cf98` | `0x55cfa4 / 0x55cfa8 / 0x55cfac` (`0x10`) | `0x102a0ba8`; `0x2a0ba8`, passes mode 2 |
| `/devmode` | `0x55cfb4` | `0x55cfc0 / 0x55cfc4 / 0x55cfc8` (`0`) | `0x1027bdf0`; `0x27bdf0` → `0x1037ea4c`; file `0x37ea4c` |
| `/testenv` | `0x55cfd0` | `0x55cfdc / 0x55cfe0 / 0x55cfe4` (`0`) | `0x1027d4f4`; `0x27d4f4` → `0x1036db68`; file `0x36db68` |

`/devunlock` occurs as a literal at A@`0x4b9ffc`, but is absent from this complete exact-route table; it is used by the shared page builder's mode-1 branch (A@`0x2a0960–0x2a0968`). This is not proof of an externally registered `/devunlock` route in 57.10. It is a table candidate in 34.16, as shown above. The audited siblings are the normal alias, manufacturing entry and legacy development alias; other maintenance routes are not represented as fully reversed here.

## Network preconditions

**N1 — The actual located route flag is a TLS requirement.** The server's policy callback is installed at A@`0x00d09c–0x00d0a8`; its setter writes server field `+464` at A@`0x426a88`. Dispatch calls it when route flags are nonzero (A@`0x430db4–0x430dd0`). Callback VA `0x1027d438` masks off the low method bits and forwards the remaining flags through VA `0x10287678` to VA `0x10286bb0` (A@`0x27d480–0x27d490`, `0x287678–0x2876b4`). Flag `0x10` takes the `0x3310` test and connection predicate; false yields 403 (A@`0x286c8c–0x286c90`, `0x286d14–0x286d28`). [Policy excerpts](evidence/unlock-20261028/http-policy-flags.dis.txt).

That predicate returns whether connection field `+36` is nonzero (A@`0x41dcf8–0x41dd14`). Accepted connections initialize it to zero, then populate it from the SSL listener's context for matching listener descriptors (A@`0x42891c`, `0x428958–0x42895c`, `0x428afc–0x428b00`). The TLS setup reads this same field before `mbedtls_ssl_init` and `mbedtls_ssl_setup` (A@`0x42b4d4`, `0x42b4fc`, `0x42b51c`). Thus this is a TLS-context check, not a comparison of the remote peer to `127.0.0.1`. [Connection setup](evidence/unlock-20261028/accepted-connection.dis.txt), [TLS initialization](evidence/unlock-20261028/tls-context.dis.txt), [predicate](evidence/unlock-20261028/tls-predicate.dis.txt).

**N2 — The located IPv4 listeners bind to the wildcard address.** Listener construction calls the bind helper with a null address argument (A@`0x425564–0x42556c`). The helper constructs AF_INET, the supplied port and zero `sin_addr`, then calls `bind` (A@`0x41ec4c–0x41eca0`). Server normal/SSL/secure-handheld listener construction uses this helper (A@`0x425690–0x4257a8`). The captured configuration gives HTTP port 1400, SSL 1443 and secure-handheld SSL 1843 (`R/opt/conf/anacapa.conf:4,7,10`). This rules out a loopback-only bind in the reviewed construction path; it is not an audit of every firewall, TLS client-auth policy or interface filter. [Bind](evidence/unlock-20261028/socket-bind.dis.txt), [listeners](evidence/unlock-20261028/http-listeners.dis.txt).

**N3 — Host validation is a separate check.** The Host helper compares the configured hostname or local connection address; `localhost` is accepted in its loopback-local-address case (A@`0x423dec–0x423ed8`). Its wrapper supplies request Host and connection-local fields (A@`0x423f04–0x423f64`); rejection becomes 400 at A@`0x425358–0x425364`. It does not make a remote connection local. [Host check](evidence/unlock-20261028/host-check.dis.txt), [caller](evidence/unlock-20261028/host-check-caller.dis.txt).

**N4 — The prior `0x4000` proof is wrong.** VA `0x1041e3e8` calls `__xstat` (A@`0x41e414`). The static-file handler passes a stat buffer at stack `+4104`, then tests mode at `+4120` with `0x4000`; the directory branch sends 403 (A@`0x427f40–0x427f74`, `0x428028`). This is a directory-mode test. It contradicts `P/UNLOCK-OPEN-QUESTIONS.md:8–14`; the prior ZP5S HTTP observation at that file's lines 19–23 cannot decide TLS versus loopback or DevMode policy in a different binary. [Stat helper](evidence/unlock-20261028/stat-helper.dis.txt), [directory branch](evidence/unlock-20261028/static-directory-check.dis.txt).

**N5 — There is also a general accepted-peer predicate, broader than loopback.** The accept wrapper obtains the peer address from `accept` and the local address from `getsockname` (A@`0x41f248–0x41f2bc`). Its caller stores the peer at connection `+156` and the local address at `+164` (A@`0x42889c–0x4288bc`, `0x428c2c–0x428c94`), then passes the peer to VA `0x10428244`. A false result branches away before the request callback (A@`0x428cc4–0x428cf0`). [Accept addresses](evidence/unlock-20261028/peer-accept-addresses.dis.txt), [predicate caller](evidence/unlock-20261028/peer-filter-caller.dis.txt).

The predicate immediately allows `169.254/16`, `10/8`, `172.16/12` and `192.168/16` (A@`0x428244–0x4282a0`). Otherwise it reads runtime address/mask words at VA `0x1067b864/0x1067b868`: if `address & mask` is zero it retains allow; otherwise it allows a matching masked peer or exact `127.0.0.1` (A@`0x4282bc–0x4282f4`). Initialization and runtime values of those two words are not established here. This is a shared address-scope predicate, not an `/unlock` loopback-only route rule; together with TLS and Host validation it constrains how the wildcard-bind finding can be interpreted. [Complete predicate](evidence/unlock-20261028/peer-address-predicate.dis.txt).

## Normal unlock

All U claims below refer to [unlock.dis.txt](evidence/unlock-20261028/unlock.dis.txt), with supporting library/script evidence linked separately.

**U1 — Methods, state and challenge.** Entry VA `0x102a0bb0` selects mode 0 and common entry VA `0x102a0a7c`. Method 1 goes to the GET page; method 4 requires `application/x-www-form-urlencoded`, otherwise the entry emits 405 (A@`0x2a0a7c–0x2a0b08`). GET holds a mutex and requires a clock-seconds difference of at least 3; otherwise it returns 403 (A@`0x2a075c–0x2a0788`, `0x2a088c`; float32 3.0 at A@`0x4979c0`). Mode 0 skips the manufacturing counter check (branch A@`0x2a0798` to `0x2a0894`).

GET tests the existing temporary flag and otherwise requests 16 bytes through `ZZ0071`, displays `02:` followed by hexadecimal nonce bytes, and sets its armed byte to 1 (A@`0x2a0894–0x2a08b0`, `0x2a096c–0x2a0998`). `ZZ0071` calls `mbedtls_ctr_drbg_random` (C@`0x71f8–0x7258`; [RNG](evidence/unlock-20261028/rng.dis.txt)); hex conversion is in U@`0x2ed8–0x2fec` ([serialization](evidence/unlock-20261028/serialization.dis.txt)). The challenge page uses status **423** (A@`0x2a0820`, wrapper `0x2a09fc–0x2a0a58`); an already-unlocked flag produces a 200 page instead (A@`0x2a0910–0x2a092c`). The flag helper stats `/tmp/device_unlocked_flag` (A@`0x37d048–0x37d088`; literal A@`0x4b9f2c`; [reader](evidence/unlock-20261028/unlocked-flag-reader.dis.txt)).

The state is process-global BSS at VA `0x105817d0`: armed byte `+0`, nonce `+4` for 16 bytes, seconds timestamp `+0x14`, mutex `+0x18` (address materialization/accesses A@`0x2a070c–0x2a0730`, `0x2a076c`, `0x2a08a4–0x2a08b0`, `0x2a083c–0x2a0840`). It is not a per-browser session or a file-backed nonce. GET also obtains the displayed serial through a separate getter call at A@`0x2a08d4`; the HTML template is A@`0x4ba03c` ([constants](evidence/unlock-20261028/constants.json)).

**U2 — Submitted value and signed bytes.** Mode-0 POST requires armed state (A@`0x2a0560–0x2a0570`). It reads up to 2048 body bytes with timeout argument 1000 (A@`0x2a0580–0x2a0588`). The parser rejects age greater than 600 seconds and requires literal `confirm=` (A@`0x2a0474–0x2a04fc`; float32 600.0 at A@`0x4983e0`; literal A@`0x4b9f8c`). The worker performs form decoding, whitespace handling and hex deserialization before verification (A@`0x29ffe8–0x2a00d8`; U@`0x2ff0–0x318c`). These are code paths, not a generated or submitted response.

The normal branch hashes **six raw bytes from object `+288`, followed by the sixteen raw nonce bytes** (A@`0x2a020c–0x2a025c`). The object's MDP buffer begins at `+268` and is loaded through `hal_flash_get_mdp_wrapper` (A@`0x355d58–0x355d78`; [object construction](evidence/unlock-20261028/manufacturing-object.dis.txt)). Therefore the buffer-relative expression is `SHA-256(MDP[0x14:0x1a] || nonce[16])`, using a half-open range. SHA-256 selection/update/finalization are confirmed by C@`0x51a8–0x535c` ([hash wrappers](evidence/unlock-20261028/hash.dis.txt)). The displayed serial text and ASCII `02:` prefix are not the two input buffers passed to these hash updates.

A@`0x2a0274–0x2a0284` passes the 32-byte digest and scheme 1 to `ZZ0109`; C@`0x65f8–0x6714` maps this to SHA-256/RSASSA-PKCS1-v1_5 verification, checks signature length against RSA modulus length and calls `mbedtls_rsa_rsassa_pkcs1_v15_verify`. Verification failure returns 403 (A@`0x2a0288–0x2a028c`, `0x2a03fc–0x2a0404`). [RSA wrapper](evidence/unlock-20261028/rsa.dis.txt).

**U3 — What the key evidence does and does not show.** Key-loader VA `0x1029ff8c` calls VA `0x103763f0`, then `ZZ0101` (A@`0x29ffa8–0x29ffbc`). The former copies 260 bytes from runtime VA `0x10676b78` if nonempty (A@`0x37640c–0x376480`; [slot reader](evidence/unlock-20261028/unlock-key-slot.dis.txt)). The key bytes and initialization provenance are not recovered here. `ZZ0101` tries certificate/public-key parsing and a private-key-parser fallback, then requires a valid RSA public component (C@`0x5eac–0x60d8`, including fallback `0x5f64`; [key parser](evidence/unlock-20261028/rsa.dis.txt)). Missing/unparseable key gives 500 through A@`0x2a0264–0x2a0268` / `0x2a01f0`.

Consequently, signature verification is established; the stronger prior assertion that this device stores **only** a public key is not established by this captured file (`P/UNLOCK-OPEN-QUESTIONS.md:51–58`). A permissive parser does not prove that private material is present either. No key recovery, response generation, brute force or credential work was attempted.

**U4 — Exact program execution and filesystem effects.** After verification and the existing-flag check, the worker invokes the fixed literal `/etc/scripts/run_sshd.sh` through VA `0x1041c324` (A@`0x2a0298–0x2a02b0`; literal A@`0x4b9f70`). Its second argument is a three-descriptor **output array**, not the submitted code: the helper splits the fixed command into argv, creates pipes, writes descriptors and calls `execve` (A@`0x41c324–0x41c448`, `0x41c1bc–0x41c244`, `0x41c2cc`; [spawn helper](evidence/unlock-20261028/spawn.dis.txt)). There is no user-selected executable in this reviewed invocation.

The script creates `/jffs/persist/ssh` and `/jffs/sys/debug/ssh`, touches `authorized_keys` if absent, generates an ECDSA-256 host key if its file is absent, backgrounds `telnetd`, and executes `dropbear -R` (`R/etc/scripts/run_sshd.sh:4–13`; [numbered script](evidence/unlock-20261028/etc-scripts-run_sshd.sh.txt)). Those are fixed service commands, not installation of the form field as a program. The caller closes returned descriptors and calls `waitpid`, but neither checks its exit status nor makes spawn failure a prerequisite for flag creation (A@`0x2a02b4–0x2a0300`). Thus a 200 response does not certify that either daemon is listening.

It next opens `/tmp/device_unlocked_flag` with create flag `0x40` and mode `0644`, closes it, and attempts `symlink("/opt/htdocs_locked", "/tmp/htdocs_locked")`; EEXIST is accepted (A@`0x2a0300–0x2a0364`, literals A@`0x4b9f2c`, `0x4b9f48`, `0x4b9f5c`). Other flag/link errors can yield 500 **after** service startup was attempted; no rollback appears in that sequence. It does not check that a pre-existing link has the expected target.

**U5 — Reboot, persistence and reversibility.** The responder initializes stack byte `r1+2056` to zero (A@`0x2a051c`, `0x2a054c`), passes its address to the complete parser at VA `0x102a0448` (A@`0x2a0624–0x2a0630`), and that parser only stores zero through it (A@`0x2a0448–0x2a0514`, store `0x2a0470`). The worker gets no pointer to that byte. The subsequent nonzero test therefore does not select the reboot branch in ordinary defined execution (A@`0x2a0634–0x2a0640`); success clears armed state/nonce and returns the 200 page via A@`0x2a0644–0x2a06d0`. The call to `sonosForceReboot` at A@`0x2a06fc` is present but unselected by this dataflow. The library reboot routine itself does call reboot (U@`0x6820–0x6864`; [reboot body](evidence/unlock-20261028/force-reboot.dis.txt)). This closes the prior “immediate forced reboot destroys the services” theory for A (`P/UNLOCK-OPEN-QUESTIONS.md:32–47`).

`R/tmp` points to `/ramdisk/tmp`, and `R/var` to `/ramdisk/var` (manifest.json:131–135 symlink metadata); `R/etc/Configure:7–16` mounts ramfs and JFFS2. Hence the flag/link and process lifetime are volatile under the stock boot layout, while the script's SSH directories/files live on persistent JFFS. Boot starts the service script automatically only under a separate `/etc/unlocked_build_flag` or `/jffs/system/Configure.dev` condition (`R/etc/Configure:86–96`). Normal mode 0 plus the fixed script creates neither hook (complete worker A@`0x29ffe8–0x2a0444`; script lines 1–13). A different pre-existing `/jffs/Configure` can replace stock boot behavior (`R/etc/Configure:57–60`).

A stock reboot without those separate hooks is therefore the static rollback mechanism for the temporary state and launched processes; SSH file changes remain. Deleting newly created SSH files would be a separate, authorized cleanup operation and was not performed. Atomicity and power-loss durability are not established.

## Manufacturing sibling

**M1 — Different approval and persistent effect.** `/mfgunlock` passes mode 2 at A@`0x2a0ba8`; it shares the form wrapper, but not the mode-0 signing branch. Its GET path reads MDP word `+0x88`, rejects `0xffffffff` with 503, and formats the transformed fuse value (A@`0x2a079c–0x2a07e0`, `0x2a093c–0x2a095c`). This limit is **not** on the normal mode-0 GET branch. Nonzero-mode POST does not use the normal armed-byte prerequisite (A@`0x2a0558–0x2a0574`), although the shared age/form parser still applies.

The worker invokes `ZZ0161`, then reads MDP/MDP2, replaces MDP word `+0x88` with a verifier output, and calls `hal_flash_mdp_write` (A@`0x2a00e0–0x2a01c0`, `0x2a03a4–0x2a03e0`; [unlock worker](evidence/unlock-20261028/unlock.dis.txt)). `ZZ0161` checks an envelope prefix, six device bytes and a fuse-derived value before delegating validation to `ZZ0160` (C@`0xc170–0xc2f4`; [envelope](evidence/unlock-20261028/mfg-envelope.dis.txt)); that nested envelope's complete signing scheme is not closed here. Result flags `&8` can additionally enter the same service-start path (A@`0x2a03e4–0x2a03f0`).

This is a persistent manufacturing-data write, not a firmware-image installation. Exact fuse encoding, the HAL's physical commit semantics and reversal requirements remain unverified; no safe automatic reversal is claimed. Do not equate its approval blob with normal `confirm` signature bytes (different calls above).

## DevMode

**D1 — Entry and input validation.** `/devmode` branches from VA `0x1027bdf0` to VA `0x1037ea4c` (A@`0x27bdf0`; [entry](evidence/unlock-20261028/devmode-entry.dis.txt)). The module must be initialized; otherwise 500. It serves GET, accepts form-encoded POST, returns 405 for other methods and 400 for missing/wrong POST content type (A@`0x37ea4c–0x37eb20`). Its POST reads 2048 bytes with timeout argument 2000 after a clock-based delay (A@`0x37e940–0x37e9e0`). [Complete module excerpt](evidence/unlock-20261028/devmode.dis.txt).

The form parser recognizes `statement`, `button`, and `csrfToken`; `csrfToken` must pass the stored-token check before install or delete (A@`0x37e6f8–0x37e85c`, especially `0x37e7b8–0x37e7d0`; literals A@`0x51e164`, `0x45ad0c`). It base64-decodes `statement` with a 1024-byte output bound, then calls the statement installer; invalid encoding gives 400 and failed installation gives 403 (A@`0x37e864–0x37e904`). Merely retrieving the form does not prove entitlement to activate DevMode.

**D2 — Statement verifier.** The installer calls `ZZ0137` before accepting/copying the bytes (A@`0x37dcfc–0x37dda0`). `ZZ0137` checks magic `0x28721e71`, model equality, minimum version `<=0x0039000a`, maximum version `>0x00390009`, and device-ID length/content equality (C@`0xa448–0xa57c`). It bounds the following bitmap, parses the trailing envelope with `ZZ0121`, and validates it with `ZZ0123` (C@`0xa580–0xa5fc`; [verifier](evidence/unlock-20261028/devmode-verifier.dis.txt)). These raw version words are recorded as compared; they are not a promise of cross-version compatibility. The complete callback-selected trust chain and key inventory are follow-ups, so this report does not assert that DevMode uses the same RSA key/envelope as `/unlock`.

**D3 — Temporary versus persistent storage.** Signed bitmap bit 0 controls persistence: if set, the installer writes the persistent copy first; if clear, it removes the persistent copy, then writes the temporary copy (A@`0x37ddc0–0x37de60`; bit accessor C@`0xa600–0xa630`). The stored payload is the decoded binary statement passed to the writer, not an XML `<DevMode>` wrapper (A@`0x37dd90–0x37ddbc`, `0x37dde0–0x37ddf0`, `0x37de38–0x37de48`).

The writer uses `/tmp/devmode.tmp.bin` for temporary state and `%s/debug/devmode.bin` for persistent state (A@`0x37e23c–0x37e338`; literals A@`0x51df90`, `0x51df6c`). The root comes from imported `g_pszJFFSRoot` at runtime VA `0x1057200c` (A relocation file `0x4f34`); U's relative relocation at file `0x26e8` initializes it from `/jffs` at U@`0x87b8`. Thus the **default** persistent path is `/jffs/debug/devmode.bin`, not `/jffs/sys/debug/devmode.bin`. A JFFSRoot configuration override is possible (A@`0x0093d0–0x009400`; U@`0x3c40–0x3ca0`; captured `R/opt/conf/anacapa.conf:51` is commented). [Relocations](evidence/unlock-20261028/relocations.json), [root setter](evidence/unlock-20261028/jffs-root-setter.dis.txt).

The writer uses open flags `0x241` (write/create/truncate), mode `0600`, a write loop and close (A@`0x37e308–0x37e3d0`); this is not an atomic rename/fsync persistence guarantee. The delete action clears state and attempts unlink of both paths (A@`0x37e7cc–0x37e7d0`, `0x37da3c–0x37db20`, `0x37e3fc–0x37e4b8`). It does not restore a previously overwritten statement. Module initialization calls the persistent reader, which revalidates with `ZZ0137` and requires bit 0 (A@`0x346b98`, `0x37dc64–0x37dccc`, `0x37db24–0x37dc60`; [startup](evidence/unlock-20261028/devmode-startup.dis.txt)).

**D4 — Effect on policy.** Successful POST calls the debug-property refresh at VA `0x1035ae78` (A@`0x37ea30`). Property permission VA `0x1035abf0` allows a temporary unlocked flag, a special property-42 exception, or the relevant verified DevMode bit via VA `0x1037deb8` (A@`0x35ab48–0x35ac50`). [Property gates](evidence/unlock-20261028/debug-property-gates.dis.txt). This establishes configuration entitlements. No program payload is passed to an exec call in the reviewed DevMode module; downstream property callbacks have not been exhaustively audited for all secondary effects.

## TestEnv

**T1 — Independent feature gate.** Entry VA `0x1027d4f4` obtains the settings interface and forwards to VA `0x1036db68` (A@`0x27d4f4–0x27d588`; [entry](evidence/unlock-20261028/testenv-entry.dis.txt)). Before GET/POST handling, it calls property permission for index **40**, returning 403 on denial (A@`0x36db68–0x36db94`, `0x36dc6c`). Property 40 is `O_SONOS_CLOUD_API_TEST_ENV` (initializer A@`0x00727c–0x0072c0`, literal A@`0x51a234`). Its bitmap index field is **7**, initialized through r8 at A@`0x006e4c` and stored at `0x0072d4`; property lookup reads `+12` and passes it to the DevMode bit accessor (A@`0x35ab50–0x35ab70`). Property number 40 is therefore not signed bit 40. An unlocked flag also satisfies the shared permission helper (D4). [Initialization](evidence/unlock-20261028/config-defaults.dis.txt).

**T2 — Operation.** GET builds a selection form; POST accepts an environment and optional URL/reset action, then sets two named properties: `OnlineUpdateBaseURL` (index 42) and `O_SONOS_CLOUD_API_TEST_ENV` (index 40) through one interface call (A@`0x36d4e4–0x36d8bc`, especially `0x36d6fc–0x36d7ac`; [handler](evidence/unlock-20261028/testenv.dis.txt)). POST requires Content-Length, rejects declared length above 1023 with 413, and reports missing length as 411 (A@`0x36d52c–0x36d568`, `0x36d7d4`, `0x36d834`). Reset supplies empty/null values through the same transaction (A@`0x36d6f0–0x36d728`, `0x36d89c–0x36d8bc`). A successful setter returns a 200 page, failure 500 (A@`0x36d7b0–0x36d7cc`). This changes configuration; no update image is supplied by this form's reviewed parser.

**T3 — Persistent destination, with the virtual call resolved.** The settings object's constructor installs vtable VA `0x104b7250` and interface vtable VA `0x104b7294` at object `+8` (A@`0x28d164–0x28d178`; vtable file `0x4b7294`, method `+12` at `0x4b72a0` → VA `0x1028caf0`). That thunk subtracts 8 and enters VA `0x1028c924`, whose virtual method `+0x34` points to VA `0x1028dfc8` (vtable file `0x4b7284`; A@`0x28ca4c–0x28ca70`). Object construction/wiring is preserved at A@`0x0ac590–0x0ac62c`, `0x1da494–0x1da4d0`, `0x28d1a4–0x28d230`. [Wiring](evidence/unlock-20261028/settings-wiring.dis.txt), [forwarding](evidence/unlock-20261028/settings-forward.dis.txt), [constructor](evidence/unlock-20261028/sysprops-constructor.dis.txt), [setter](evidence/unlock-20261028/sysprops-setter.dis.txt), [vtable words](evidence/unlock-20261028/constants.json).

That concrete method applies the file transaction and notifies changed settings (A@`0x28dfc8–0x28e0d4`; [commit](evidence/unlock-20261028/sysprops-commit.dis.txt)). The default directory is `/jffs/settings` (A@`0x006c70–0x006c80`, literal `0x4b5508`), constructor filename is `syssettings.xml` (A@`0x28d12c–0x28d160`, literal `0x4b7478`), joined into object `+64` by A@`0x28cfd0–0x28cfec`. The transaction constructs `%s.tmp` and renames it to the stored destination (A@`0x28c108–0x28c138`, `0x28c224–0x28c328`; literal A@`0x45b0e8`; [transaction](evidence/unlock-20261028/sysprops-transaction.dis.txt)). Thus the default persistent destination is **`/jffs/settings/syssettings.xml`**, subject to configured root overrides (A@`0x00948c–0x0094b4`, format literal `0x45b390`). Reversibility is the same form's reset/restore operation, not a rollback of all prior file contents; power-loss durability and subsequent network use of changed settings were not exercised.

## Root authentication is separate

**R1 — Service start is not authentication success.** The unlock code launches the fixed script (U4), while Dropbear obtains the selected account with `getpwnam`, takes UID/GID/home/shell, and prefers `getspnam`'s stored password field if available (D@`0x9738–0x9844`; [account loader](evidence/unlock-20261028/ssh-account-loader.dis.txt)). The authentication dispatcher rejects an unknown account; checks effective UID versus requested UID when not privileged; conditionally rejects UID 0 under its root-login policy; applies an optional group restriction; and checks the account shell with `getusershell` (D@`0x130a0–0x13288`; [policy](evidence/unlock-20261028/ssh-account-policy.dis.txt)). The root-rejection diagnostic is linked by D relocation file `0x31dc` to D@`0x2add9`, corroborating the UID-0 branch at `0x13100–0x13128`.

For the password method, it rejects oversized input (>100), computes `crypt(candidate, stored_hash)`, checks stored/computed lengths and compares their contents before success (D@`0x133ec–0x13588`, especially `0x1345c–0x1346c`, `0x134c0–0x13534`; [password path](evidence/unlock-20261028/ssh-password.dis.txt)). Blank stored hashes are rejected in this password-method path; a separate `none`-method blank-password policy exists at D@`0x13290–0x13324`, so unconditional claims about all authentication modes would be wrong. Password/root-method availability depends on option fields checked at D@`0x13328–0x13370`. Public-key authentication is dispatched separately at D@`0x13378–0x1339c`; its entire authorization chain was not reversed here.

The fixed script prepares an `authorized_keys` file but installs no submitted public key (`R/etc/scripts/run_sshd.sh:6–13`); Dropbear contains the corresponding path literal at D@`0x2afb0`. Nothing in these reviewed paths converts the `confirm` signature into the root password or a public key. Actual root account contents, runtime option values and credentials are deliberately unverified: no account database was inspected for this report, no credentials were submitted, and no cracking was attempted. Telnet's complete login implementation is also outside this completed trace; script line 12 establishes only its launch command.

Privilege in the matrix means the service process's inherited authority, not authority conferred on the HTTP client: stock init runs `/etc/Configure` and `/etc/runanacapa` (`R/etc/inittab:2,5`), and the worker's child inherits process execution context in the reviewed fork/exec helper (A@`0x41c1fc–0x41c2cc`). Root runtime identity on the actual ZP5S is not freshly verified.

## Entry-point matrix

Each evidence ID refers to the binary/file offsets above. **Applicability for every row is A's captured Fenway build; exact ZP5S behavior remains unverified.** Safest next steps below are offline artifact work, not authorization to contact hardware.

| Interface | Source evidence | Applicability | Privilege | Persistence | Reversibility | Confidence | Safest next verification |
|---|---|---|---|---|---|---|---|
| `/unlock`, `/unlock.htm` | Route file `0x55cf60/0x55cf7c`; N1–N5; U1–U5 | 57.10 A; old candidates differ | Daemon's inherited authority; fixed child script; root auth separate (R1) | RAM flag/link/processes; JFFS SSH files remain | Stock reboot clears volatile state absent separate boot hooks; SSH files need separate cleanup | High bounded static; target unknown | Repeat complete trace on matching captured ZP5S A/C/U/scripts; resolve runtime key-buffer initialization without secrets |
| `/mfgunlock` | Route file `0x55cf98`; M1 | 57.10 mode 2; 34.16 separate candidate | Daemon calls flash HAL after envelope approval | MDP `+0x88` write; optional U4 effects | No verified reversal | High for call/write; nested approval/physical commit incomplete | Trace `ZZ0160` and HAL from already-captured code; require provenance/backup design before any separate future device proposal |
| `/devunlock` | A literal `0x4b9ffc`, complete routes; version table | No exact A entry; 34.16 alias candidate | Not established for A as external route | Do not infer from a literal | Unknown | High bounded absence; legacy behavior unclosed | Analyze 34.16 registration/policy/handler with its own mappings |
| `/devmode` | Route file `0x55cfb4`; D1–D4 | 57.10 A, model/version/ID verifier constraints | Daemon stores entitlement statement; CSRF + verifier | Temp file always on accepted installation; persistent copy only for signed bit 0 | Delete action attempts both unlinks; previous statement not restored | High for local flow/files; full trust chain and indirect effects incomplete | Resolve verifier callbacks and property consumers in matching captured userspace |
| `/testenv` | Route file `0x55cfd0`; T1–T3 | 57.10 A, property 40 / signed bit 7 or unlocked flag | Daemon's settings transaction | Default JFFS `settings/syssettings.xml` | Reset/restore through same gated setter; no historical backup | High static wiring; runtime not exercised | Compare matching target vtables/defaults and downstream setting consumers offline |
| SSH password after U4 | D@`0x9738`, `0x12f4c`, `0x133ec`; R1 | Captured D/script only | Account UID after independent auth; root option/shell/group policy | Host/authorized-key files persistent; process start volatile | Stop/reboot services; SSH file cleanup separate | High for password comparisons; actual account/options unknown | Inspect non-secret option initialization and public-key code in captured D; no credentials needed |

## Execute versus install

For normal `/unlock`, the complete reviewed worker and script perform fixed program execution, runtime flag/link changes and SSH file preparation (U4–U5). They contain no image payload parser or call to a firmware-image installer in this mode. `/mfgunlock` is a separate persistent MDP write (M1), `/devmode` stores a validated statement (D3), and `/testenv` changes persistent XML settings (T3). None establishes an arbitrary firmware installation path, bootloader unlock, signature bypass, or arbitrary user-program upload. Official-update signature enforcement is a supplied premise, not newly audited here (`/home/ben/.review-notes/2026-10-28-sol-burn/L1-moggie-unlock.md:10`).

## Negative-results register

| Dead end / unsupported claim | Bounded result and evidence | Avoid repeating |
|---|---|---|
| `0x4000` proves loopback-only | Directory-mode check, N4 | Follow the stat buffer; inspect actual route flags/TLS predicate. |
| A 403 distinguishes local-only from other policy | TLS predicate, GET rate limit and TestEnv permission independently yield 403 (N1, U1, T1) | Do not infer a socket peer restriction from status alone. |
| Success immediately forces reboot | Outflag is zero throughout ordinary parser flow, U5 | Follow writes to the condition, not merely the presence of a reboot call. |
| Code becomes a script argument/program | Second argument is output descriptors; command literal fixed, U4 | Follow helper calling convention through execve. |
| 200 proves daemon startup | Spawn failure and child exit status do not gate flag creation, U4 | Describe attempts separately from runtime listening state. |
| Normal unlock has the manufacturing fuse exhaustion limit | Mode 0 jumps over that block, U1/M1 | Keep mode branches separate. |
| Every DevMode statement persists / is XML | Signed bit 0 and raw-byte writer, D2–D3 | Inspect writer arguments and persistence bit. |
| Persistent DevMode path is `/jffs/sys/debug/...` | Imported default root gives `/jffs/debug/devmode.bin`, D3 | Apply copy/relative relocations; do not guess root from neighboring SSH paths. |
| TestEnv persistence is `localsettings.txt` | Resolved vtable reaches `syssettings.xml` transaction, T3 | Resolve the actual interface before assigning a filename. |
| Property 40 means signed bit 40 | Field maps it to bit 7, T1 | Distinguish property indices from entitlement bitmap indices. |
| Firmware contains only the unlock public key | Runtime bytes absent; parser accepts several key formats, U3 | Do not claim absence or presence of private material from parser names. |
| String hit means registered route / live 404 proves absent code | Complete A table and registration; `/devunlock` literal has no A exact entry | Preserve table base/stride and distinguish build/runtime evidence. |
| All directories are PPC; big-endian pointer search is universal | 28.1 ELF machine/endian differs, version inventory | Read ELF identity before interpreting pointers or instructions. |
| Missing `run_sshd.sh` proves no old maintenance services | Exact file absent in four inventoried roots only | Older startup/crypto behavior remains a separate analysis. |
| Matching version number proves matching ZP5S image | Fenway inittab vs prior ZP5S identification; scope section | Require model-family provenance and matching userspace. |
| No magic hits proves the ZP5S UPD is encrypted/unextractable | Signature search only, target-package.json | A format-aware offline extractor/provenance is still needed. |
| TokenSave can answer this capture immediately | Attempted semantic context call did not return; this worktree has no `.tokensave/tokensave.db` | Use bounded direct ELF analysis for this session; do not invent graph evidence. |
| Broad recursive scan of `/home/ben/sonos-bench` is a clean firmware inventory | It encounters emuroot-mounted/protected host paths | Restrict scans to `sonos-fenway/firmware` and `sonos-bench-analysis`; do not traverse emuroot. |

## Reproduction and verification

The generator reads only selected ELF files, five non-secret stock text files, symlink metadata and the optional UPD. It invokes the existing **host** GNU objdump; it never executes a captured program, submits a request, emulates a target or modifies captured files ([tools/stock_unlock_evidence.py](../../tools/stock_unlock_evidence.py)). Exact objdump commands appear at the start of every `.dis.txt` excerpt.

Executed from this worktree:

```sh
python3 tools/stock_unlock_evidence.py \
  --rootfs /home/ben/sonos-bench/sonos-fenway/firmware/rootfs/57.10-25140 \
  --objdump /home/ben/sonos-bench/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin/powerpc-buildroot-linux-gnu-objdump \
  --target-package /home/ben/sonos-bench-analysis/zp5s/57.23-74170-1-16.upd \
  --out docs/hardware/evidence/unlock-20261028
```

Verification results and the ranked handoff are in [2026-10-28-l1-moggie-unlock.md](../reviews/2026-10-28-l1-moggie-unlock.md). The authorization boundary remains offline only (brief lines 29–40). Required next input is **already-captured matching ZP5S userspace plus provenance**, not device access, credentials, flashing or GPIO.
