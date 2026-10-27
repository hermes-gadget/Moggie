# Contributing

## The short version

1. Open an issue first — even a small one. Describe the problem, not the patch.
2. Branch from `main`.
3. One logical change per pull request.
4. Squash merge. The branch gets deleted.

No direct pushes to `main`.

## Branch names

```
feat/kernel-build
fix/ota-slot-verify
docs/hardware-notes
chore/toolchain-bump
```

## Commits

Conventional prefixes: `feat:`, `fix:`, `docs:`, `chore:`, `test:`, `refactor:`.

The body should say *why*, not just what. If a change is not obvious, explain the
constraint that forced it.

## Pull requests

Every PR description answers:

- What changed, and why
- How it was verified (the exact command, and the result)
- What it does **not** do

CI must be green. A PR that "should work" is not a PR that works.

## Hard rules

**The rootfs budget.** The image must stay ≤ 7.5 MB. This is not a preference — it is
what makes the A/B flash layout possible. A feature that doesn't fit gets cut, not
squeezed.

**Never commit build output.** `out/`, `dl/`, `toolchain/`, kernel images, and NAND dumps
are all in `.gitignore`. Keep them out.

**Never commit anything from a real device.** No firmware dumps, no keys, no serial
numbers, no MAC addresses. If you need to share a log, strip it first.

**Don't touch the bootloader.** mtd0 is the only unbrick path. Code that writes to it
will not be merged.

## Licensing

Firmware contributions are GPL-2.0. Tooling contributions are MIT. By opening a PR you
agree to license your contribution under the same terms as the directory it touches.

## Testing

```bash
make check
make ota-test
```

The OTA negative tests are the important ones. A change that makes updates less safe is a
regression even if it adds a feature.

## Reporting hardware results

If you've run Moggie on real hardware, open an issue with:

- model and firmware version you started from
- what you flashed and how
- what worked, what didn't
- the boot log, with anything identifying stripped

Good hardware reports are worth more than patches.
