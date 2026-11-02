# ReaWebAPI

- Persist random IDs for ordinary WebUIs without app.json.id in a private .reawebapi/identity.json marker, independent of launcher names.
- Preserve automatic identity across moves and restarts, and assign separate identities to copied Apps when the original binding remains valid, with serialized initialization and updates.
- Replace launcher-derived fallback identities without migrating their storage. Existing data is retained, and explicit IDs, JSFX identity derivation and window instance rules remain unchanged.
