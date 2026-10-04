// Settings search (pure, no DOM). Ranks every searchable row against a query, best match first.
//
// Each row is scored from its own text, best field first: label (exact > word-prefix > substring >
// fuzzy), then its meta keywords (synonyms, never displayed), then its description, then its card
// caption and tab name. Every query word must match something (fuzzy allowed) and the per-word scores
// add up; a whole-label match and a whole-phrase keyword match add a bonus. Matching ignores case,
// accents and punctuation. Typo tolerance (same first letter): edit distance 1 for words of 4-7 letters, 2 for 8+ (none
// below 4), a typo'd prefix counts too, and a few spelling folds (ight/ite, ph/f) catch phonetic
// spellings such as "nite". Advanced rows also match the word "advanced".
//
// search(groups, query) -> flat array, best first:
//   { key, row, label, desc, caption, groupId, groupLabel, adv, score }
// Rows with no label (the About hero) are not searchable. The Tray menu's item lists are not schema
// rows (they are drawn by src/tray), so they never appear; its "Performance in the tray" switch is a
// normal row and does.

export const words = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '')
  .toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);

// Optimal string alignment distance (insert, delete, substitute, adjacent swap), early exit past max.
export function editDistance(a, b, max = 2) {
  if (a === b) return 0;
  if (Math.abs(a.length - b.length) > max) return max + 1;
  let prev2 = null, prev = Array.from({ length: b.length + 1 }, (_, j) => j);
  for (let i = 1; i <= a.length; i++) {
    const cur = [i];
    let rowMin = i;
    for (let j = 1; j <= b.length; j++) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      let v = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost);
      if (prev2 && i > 1 && j > 1 && a[i - 1] === b[j - 2] && a[i - 2] === b[j - 1]) v = Math.min(v, prev2[j - 2] + 1);
      cur[j] = v;
      if (v < rowMin) rowMin = v;
    }
    if (rowMin > max) return max + 1;
    prev2 = prev; prev = cur;
  }
  return prev[b.length];
}

// How many typos a query word may carry.
export const budget = (w) => (w.length >= 8 ? 2 : w.length >= 4 ? 1 : 0);
// Spelling folds for the fuzzy comparison only.
const fold = (w) => w.replace(/ight/g, 'ite').replace(/ph/g, 'f');

// Score of one query word against one list of target words, as a 0..1 strength (0 = no match):
// exact 1, word-prefix .8, substring .6, fuzzy (whole word or typo'd prefix) .45.
function wordStrength(q, targets, fuzzy = true) {
  let best = 0;
  const b = fuzzy ? budget(q) : 0, fq = fold(q);
  for (const t of targets) {
    let s = 0;
    if (t === q) s = 1;
    else if (t.startsWith(q)) s = 0.8;
    else if (q.length >= 3 && t.includes(q)) s = 0.6;
    else if (b > 0 && t[0] === q[0] && !STOPWORDS.has(t)) {   // fuzzy needs the same first letter: far fewer false hits
      const ft = fold(t);
      if (editDistance(q, t, b) <= b || editDistance(fq, ft, b) <= b) s = 0.45;
      else if (q.length >= 5 && t.length > q.length && editDistance(q, t.slice(0, q.length), b) <= b) s = 0.4;
    }
    if (s > best) best = s;
    if (best === 1) break;
  }
  return best;
}

// Field weights: what a full-strength hit in that field is worth.
const W = { label: 100, kw: 40, desc: 18, cap: 8 };

const cache = new WeakMap();   // row -> its normalized fields (rows are static schema objects)
function fieldsOf(row, card, group) {
  let f = cache.get(row);
  if (f) return f;
  const label = words(row.label);
  f = {
    label,
    compact: label.join(''),
    labelNorm: label.join(' '),
    kw: (row.keywords || []).flatMap(words),
    kwPhrases: (row.keywords || []).map((k) => words(k).join(' ')),
    desc: words(row.desc),
    cap: [...words(card.caption), ...words(group.label), ...(row.adv ? ['advanced'] : [])],
  };
  cache.set(row, f);
  return f;
}

// Common short words carry no meaning of their own: dropped from a query that has other words.
const STOPWORDS = new Set(['in', 'the', 'to', 'of', 'a', 'an', 'with', 'for', 'on', 'and', 'or', 'is', 'at', 'these', 'this', 'that', 'those', 'from', 'your']);
const meaningful = (q) => { const m = q.filter((w) => !STOPWORDS.has(w)); return m.length ? m : q; };

// Score one row for the query words (0 = no match).
function scoreRow(f, all) {
  const q = meaningful(all);
  let total = 0;
  // Stopwords never need to match; an exact label hit only breaks ties ("zoom in" -> Zoom in over Zoom out).
  if (q.length < all.length) for (const w of all) if (STOPWORDS.has(w) && f.label.includes(w)) total += 5;
  for (const w of q) {
    let best = 0;
    const l = wordStrength(w, f.label) * W.label;
    best = Math.max(best, l);
    if (w.length >= 4 && f.compact.includes(w)) best = Math.max(best, 0.55 * W.label);   // "zoomin" for "Zoom-in"
    if (best < W.label) {
      best = Math.max(best, wordStrength(w, f.kw) * W.kw);
      // Free text (description, caption, tab name) matches exactly/by prefix/substring only: fuzzy hits there
      // pair unrelated words that merely look alike (tray/trac, theme/these).
      best = Math.max(best, wordStrength(w, f.desc, false) * W.desc);
      best = Math.max(best, wordStrength(w, f.cap, false) * W.cap);
    }
    if (best === 0) return 0;   // every word must match something
    total += best;
  }
  // Tie-breaks among equal matches: a label that opens with the query beats one that merely contains it,
  // and shorter labels beat longer ones.
  if (f.label[0] && f.label[0].startsWith(q[0])) total += 10;
  total -= f.label.length * 0.5;
  const phrase = all.join(' ');
  if (f.labelNorm === phrase) total += 400;
  else if (f.labelNorm.startsWith(phrase)) total += 60;
  else if (all.length > 1 && f.labelNorm.includes(phrase)) total += 30;
  if (all.length > 1 && f.kwPhrases.some((k) => k.includes(phrase))) total += 25;
  return total;
}

export function search(groups, query) {
  const q = words(query);
  if (!q.length) return [];
  const hits = [];
  let order = 0;
  for (const g of groups) {
    for (const card of g.cards || []) {
      for (const r of card.rows || []) {
        if (!r.label) continue;
        const score = scoreRow(fieldsOf(r, card, g), q);
        if (score > 0)
          hits.push({ key: r.key, row: r, label: r.label, desc: r.desc || '', caption: card.caption || '',
                      groupId: g.id, groupLabel: g.label, adv: !!r.adv, score, order: order++ });
        else order++;
      }
    }
  }
  return hits.sort((a, b) => b.score - a.score || a.order - b.order);
}
