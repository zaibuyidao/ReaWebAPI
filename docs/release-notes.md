# ReaWebAPI

- Removed the 730 mirrored REAPER methods from the web API, along with transaction.batch, debug.setBufferSize, and the External Client methods api.call and api.batch.
- Migrated the bundled templates and examples to dedicated operations for project and track queries, gain changes, and transport control.
- Added an upgrade tool that removes obsolete mirror files matching the previous installation package and preserves locally modified files.
