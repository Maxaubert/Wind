import './theme.css';
import { mount } from 'svelte';
import App from './App.svelte';
// Input modality for the focus ring (see tokens.css): keyboard shows it, the mouse hides it.
addEventListener('keydown', (e) => { if (!['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) document.documentElement.setAttribute('data-kbd', ''); }, true);
addEventListener('pointerdown', () => document.documentElement.removeAttribute('data-kbd'), true);
document.addEventListener('dragstart', (e) => e.preventDefault());
export default mount(App, { target: document.getElementById('app') });
