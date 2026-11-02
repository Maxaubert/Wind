// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Wind for Mac applies every change at once (no profiles, no Save or Discard): the page holds one map,
// the values config.json has now, as the host reports them (strings). No bridge, no DOM, so tests can
// import it directly.
import { schemaDefaults } from './settings-schema.js';

// session: { values } as the host reports them. Returns the values with defaults filled in for any
// key the host left out (an older Wind or a test mock).
export function fill(session) {
  return { ...schemaDefaults(), ...((session && session.values) || {}) };
}
