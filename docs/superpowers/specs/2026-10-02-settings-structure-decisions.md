# Settings structure (Max, 2026-10-02)

## Settled
- Sidebar, top to bottom: **Hotkeys** (Settings opens here, it is set up first), **Zoom**, **View**, **Screen**;
  divider; **Preferences**, **Tray menu**, **About**. The bottom group sits under the list (not pinned to the
  window bottom). No EXPERT/TRAY labels, no Advanced tab.
- "Colour" renamed **Screen** (it is about screen light: warmth, brightness).
- **View** = pan speed, cursor speed, keep the mouse pointer (+ edge margin), follow text cursor, follow
  keyboard focus, keep the text cursor and focus, high resolution cursor.
- **Preferences** = Appearance (built-in theme grid: this grey one, orange sliders, Noctua-style,
  cyberpunk black + yellow, ...; themes also restyle the tray flyout), Profiles, and the
  **Show advanced settings** switch.
- Advanced settings: ONE global switch in Preferences. ON = every tab shows its advanced rows inline (an
  Advanced card at the bottom of the page). OFF = hidden everywhere; search still finds them.
- Expert items spread by topic:
  - Zoom advanced: release glide?, zoom-in ease, ease-in duration, magnifier engine, engines per window kind,
    Never use Render for.
  - Hotkeys advanced: Pass zoom keys to these apps.
  - View advanced: pan smoothing, Mouse-locked games.
  - Preferences advanced: Frametime logging, Export diagnostics, Open settings file.
- Reference mockup of the earlier round: ia04 ("Advanced on every page") was the closest.

## Still open
- Exact placement of Release glide (main or advanced) and of High resolution cursor (main or advanced).
- Appearance themes: which ones, what each changes, tray flyout theming.

## Round ia06 feedback (Max, 2026-10-02)
- Divide pages internally into more sub-sections, but a sub-section needs TWO OR MORE items; a single item
  never gets its own section (it joins a neighbour or an uncaptioned card). Hotkeys is the model: split it,
  with Hide cursor + Inspect mode as a "Cursor" sub-section.
- Preferences: the troubleshooting rows (Frametime logging, Export diagnostics, Open settings file) are ALWAYS
  shown, not tied to the advanced switch, and their section must NOT be called "Advanced" (it reads as if
  those are the advanced settings). New name e.g. "Troubleshooting".
- Cards use the real app colour #080808 (the mockup copied the older #1b1b1b reference).
- Dark / light mode moves OUT of the title bar into Preferences (next to the theme picker). The title bar
  keeps only minimize / maximize / close.
- Mode row (System / Light / Dark, three-way, default System) sits FIRST in Preferences, above the theme grid.
- Profile row: only a dropdown + "New". Delete = a trash icon on each profile in the dropdown list, with a
  confirm popup. "New" opens a popup: name + start from current settings (copy) or default settings.
  Rename and Copy buttons are gone (rename: open question).
- Hotkeys: "Zoom with the scroll wheel" is merged into Zoom in / Zoom out. Key capture also detects the
  wheel: scrolling up while capturing records "Wheel up" (with any held modifiers) for Zoom in, scrolling
  down records "Wheel down" for Zoom out (e.g. Zoom in = Mouse 5 or Ctrl + Wheel up). Core work needed at
  build time: wheel bindings per direction instead of one modifier+wheel setting (migrate the old key).
- Key binding rules (Max): Zoom in / Zoom out = up to 2 bindings each; every other hotkey = 1 binding. A
  binding is at most 2 modifiers + 1 key/button/wheel direction (modifiers optional). Pan = exactly one
  binding of 1-2 modifiers (required), shown as [Ctrl] + [Alt] + one fixed "Arrow keys" cap (not 4 caps).
  Mockup: click a binding to re-record it, hover shows a small x to remove it; wheel only on zoom rows.
- Each binding is ONE box with the + inside it, dim (e.g. [Ctrl + Wheel up]); pan is one box [Ctrl + Alt + Arrow keys] with the arrow part dimmer. Separate boxes made + read like "or".
- Hotkeys sections: "Zoom" (Zoom in, Zoom out, + advanced Pass zoom keys) and "Extra keys" (Pan with the
  arrow keys, Hide cursor, Inspect mode). Each extra key has an on/off switch on its row, right of the
  binding: off frees the keys but KEEPS the binding (shown dimmed) so on restores it. Toggle lives on the
  Hotkeys tab, not elsewhere.
- "Never use Render for" (renderExclude, the copy-protected-video app list) is REMOVED from Settings (Max
  did not approve it; the core's own detection stays).
- Copy rules (Max): names clear and clean; descriptions one short sentence or a few words saying what the
  setting does; no symbols, no product names, no comparisons. Applied in ia07 (see the copy table in chat
  2026-10-02): e.g. Hide cursor -> Hide pointer, Pan speed -> Arrow key speed, Cursor speed -> Mouse speed,
  Keep the mouse pointer -> Pointer position, Keep the text cursor and focus -> Text cursor position,
  Zoom-in ease -> Soft start, Ease-in duration -> Soft start length, Magnifier engine -> Engine,
  Pass zoom keys to these apps -> Share zoom keys with these apps, "tray flyout" -> "tray menu".
- Advanced rows carry NO marker at all (Max rejected icons and badges): the Show advanced settings switch is the only signal.
- Themes mocked up in ia08 (tokens in ia/palettes08.cjs): Wind grey (today), Ember (orange), Cocoa (the
  Noctua-style brown + beige; brand name not used), Cyberpunk (black + signal yellow, smaller radii). Each
  has designed dark AND light variants; Mode System/Light/Dark switches live; the tray flyout follows both.
- Theme review (Max): LOCKED IN = Wind grey, Ember, Cyberpunk. Cocoa removed. Cyberpunk selections must be
  solid saturated yellow (not transparent dark yellow). 27 more candidates (monochrome, colour accents,
  character themes) in ia/themes.html: step through, toggle Select, copy the picks list back to Claude.
- FINAL THEME SET (Max, 2026-10-02): Wind grey (grey), Ember (ember), Cyberpunk (cyber), Mono (b1_mono),
  Slate (b1_slate), Carbon (b1_carbon), High contrast (b1_hicon), Deep ocean (b2_ocean). Tokens in
  palettes08.cjs + palettes-b1.cjs + palettes-b2.cjs. The theme list in Settings must take ONE row (layout
  being mocked up in picker.html). Build started 2026-10-02.
- FINAL THEMES, REVISED (Max, 2026-10-02): only FOUR, in this order: Wind grey (grey), Ember (ember),
  Deep ocean (ocean), High contrast (hicon, always last). Cyberpunk, Mono, Slate and Carbon are dropped.
  With 4 themes the picker fits one row without scrolling.
- Theme picker layout (Max): option A cards (picker.html#A): one row of mini window preview cards with the
  name under each, selected card outlined; with four themes no scrolling and no arrow buttons are needed.
