# Native Four-Column Windows Snap Layout

Adds an extra native Windows 11 Snap Layout with four equal vertical columns:

`| 25% | 25% | 25% | 25% |`

The layout is integrated into Windows' own Snap Layout UI and appears in:

- **Win+Z**
- **Maximize-button hover**
- **Snap Bar** when dragging a window to the top of the screen

Because the mod extends the native Snap Layout model, snapping continues to use
Windows' normal Snap Assist and Snap Groups behavior.

## Compatibility

This mod relies on undocumented Windows internals and private symbols from
`SnapLayout.dll`. Windows updates can change these implementation details.

The mod contains structural checks before modifying a layout. If the expected
native layout structure isn't found, it leaves that layout set unchanged rather
than patching an unknown structure.

Developed and tested on Windows 11 build **26200.9445** on x86-64.

If a Windows update changes the relevant private symbols, Windhawk may be unable
to load the mod until it is updated.
