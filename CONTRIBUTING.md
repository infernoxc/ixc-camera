# Contributing

1. Build and run the tests before sending a change: `./scripts/build.ps1 -Preset debug -Test` and `-Preset release -Test`.
2. Warnings are errors. Fix the cause instead of suppressing the warning.
3. Add a regression test with every bug fix.
4. Performance on weak hardware is a feature. Hot paths must not allocate per frame, add polling, or add unbounded queues. Include before/after measurements with performance-sensitive changes.
5. Don't add dependencies without recording the reason, version, license and size impact in `THIRD_PARTY_LICENSES.md`.
6. Never commit build output, signing keys, certificates, logs or camera captures.
7. Effects must be original work. Don't submit assets extracted from other products (Snap Camera lenses, Instagram filters and so on).

By contributing you agree that your contributions are licensed under the MIT License.
