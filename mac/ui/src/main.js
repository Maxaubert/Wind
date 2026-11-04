// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
import './theme.css';
import { mount } from 'svelte';
import App from './App.svelte';
// Input modality for the focus ring (see tokens.css): keyboard shows it, the mouse hides it.
addEventListener('keydown', (e) => { if (!['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) document.documentElement.setAttribute('data-kbd', ''); }, true);
addEventListener('pointerdown', () => document.documentElement.removeAttribute('data-kbd'), true);
document.addEventListener('dragstart', (e) => e.preventDefault());
// No WebKit context menu (Reload, Look Up...): a right-click on a keybind clears it, and the rest of the
// window is an app, not a web page. Text fields keep theirs for Cut, Copy and Paste.
document.addEventListener('contextmenu', (e) => { if (!(e.target instanceof Element && e.target.closest('input, textarea'))) e.preventDefault(); });
export default mount(App, { target: document.getElementById('app') });
