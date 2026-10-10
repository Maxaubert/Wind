// Profile name rules, mirroring the host's ValidateNewName (src/config_ui/main.cpp, src/profiles.*): a
// profile is a file, so the name must be a legal Windows file name. Pure, no DOM, so tests import it.
const forbidden = /[\\/:*?"<>|]/;
const reserved = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i;
// Windows reserves a device name with any extension too ("con.x", "NUL.txt"), and ignores spaces before the dot.
const stemOf = (t) => t.split('.')[0].replace(/ +$/, '');

// '' when the name is fine, else the sentence to show under the field. `names` = the existing profiles.
export function nameError(t, names = []) {
  if (!t.trim()) return 'Enter a name for the profile.';
  if ([...t].length > 40) return 'The name is too long (40 characters at most).';
  if (forbidden.test(t) || [...t].some((c) => c.charCodeAt(0) < 32)) return 'The name contains a character a file name cannot have.';
  if (t !== t.trim() || t.startsWith('.') || t.endsWith('.')) return 'The name cannot start or end with a space or a dot.';
  if (reserved.test(stemOf(t))) return 'Windows reserves that name.';
  if (names.some((x) => x.toLowerCase() === t.toLowerCase())) return 'A profile with this name already exists.';
  return '';
}

// "Profile 2", the first free number.
export function suggestName(names = []) {
  let n = names.length + 1;
  while (names.some((x) => x.toLowerCase() === ('Profile ' + n).toLowerCase())) n++;
  return 'Profile ' + n;
}
