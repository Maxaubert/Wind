// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The menu bar panel's entry (panel.html). The page is clear: the host window is transparent and the
// card draws its own shadow.
import './panel-page.css';
import { mount } from 'svelte';
import Panel from './Panel.svelte';
// Input modality for focus rings: the keyboard shows them, the mouse hides them (as Settings, main.js).
addEventListener('keydown', (e) => { if (!['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) document.documentElement.setAttribute('data-kbd', ''); }, true);
addEventListener('pointerdown', () => document.documentElement.removeAttribute('data-kbd'), true);
document.addEventListener('dragstart', (e) => e.preventDefault());
// No WebKit context menu (Reload, Look Up...): the panel is an app surface, not a web page.
document.addEventListener('contextmenu', (e) => e.preventDefault());
export default mount(Panel, { target: document.getElementById('app') });
