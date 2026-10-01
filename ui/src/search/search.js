// Settings search (pure). Matches rows across all groups, hidden (showIf) and Advanced rows included.
// A row matches when every query word is a word-prefix of its label, description, card caption or
// group label (case-insensitive). Returns [{ id, label, rows: [{ key, label, desc, caption, groupId }] }]
// in schema order, dropping groups with no match.

const words = (s) => String(s || '').toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);

export function search(groups, query) {
  const q = words(query);
  if (!q.length) return [];
  const out = [];
  for (const g of groups) {
    const rows = [];
    for (const card of g.cards || []) {
      for (const r of card.rows || []) {
        if (!r.label) continue;
        const hay = [...words(r.label), ...words(r.desc), ...words(card.caption), ...words(g.label)];
        if (q.every((w) => hay.some((h) => h.startsWith(w))))
          rows.push({ key: r.key, label: r.label, desc: r.desc || '', caption: card.caption || '', groupId: g.id });
      }
    }
    if (rows.length) out.push({ id: g.id, label: g.label, rows });
  }
  return out;
}
