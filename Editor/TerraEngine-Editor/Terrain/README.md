# Terrain Editor

This directory owns editor-facing terrain creation, import, sculpting,
selection, preview, procedural tools, history integration, diagnostics, and
automation providers.

## Public boundary

- Route editor input and automation through typed terrain edit transactions.
- Keep drag previews detached from committed authored state.
- Report progress, cancellation, conflicts, receipts, and unavailable-target
  diagnostics through the existing editor job/automation services.

## Private boundary

- Brush kernels, masks, selection evaluation, import remapping, and editor UI
  state remain local to this module.
- Editor workers prepare candidates only; protected publication owns the commit.

Visible tools and automation must produce equivalent typed transactions,
history entries, dirty-state changes, and derived invalidation.
