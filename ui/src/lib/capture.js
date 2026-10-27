// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Keybind captures that are listening right now (Bindings, PanBinding). Settings' page shortcuts
// (Ctrl+F, Ctrl+Q, Escape) stand down while one is: its window listener runs before theirs, so the
// combination being recorded would also run the shortcut (#495).
const listening = new Set();
export function setCapturing(id, on) { if (on) listening.add(id); else listening.delete(id); }
export const capturing = () => listening.size > 0;
