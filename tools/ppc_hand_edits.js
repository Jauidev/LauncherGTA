// Re-applies the hand edits of the recompiled code, which XenonRecomp does not produce, to a fresh
// XenonRecomp output (tools/recompile.bat runs it before syncing into ppc/). Each edit names a
// function and a line inside it; the script stops when that line is not there exactly once, so a
// changed recompiler output is noticed instead of silently losing an edit.
// Usage: node ppc_hand_edits.js <XenonRecomp output dir> [function]
// function: its name in another title's output (GTA IV's sub_8285CB40 is sub_827EE370).
const fs = require('fs');
const path = require('path');

// sub_8285CB40 waits on the events (+156) of a list of RAGE tasks and then frees them (+4). When the
// task pool runs out, sub_8285D3A8 hands out null tasks, which the Xbox never did; these guards keep
// the waiter from reading and writing guest address 0.
const edits = [
  ['__imp__sub_8285CB40', [
    ['\tctx.r8.u64 = PPC_LOAD_U32(ctx.r8.u32 + 156);',
     '\tctx.r8.u64 = ctx.r8.u32 ? PPC_LOAD_U32(ctx.r8.u32 + 156) : 0;'],
    ['\tPPC_STORE_U32(ctx.r9.u32 + 4, ctx.r25.u32);',
     '\tif (ctx.r9.u32) PPC_STORE_U32(ctx.r9.u32 + 4, ctx.r25.u32);'],
    ['\tPPC_STORE_U32(ctx.r11.u32 + 4, ctx.r25.u32);',
     '\tif (ctx.r11.u32) PPC_STORE_U32(ctx.r11.u32 + 4, ctx.r25.u32);'],
  ]],
];

const dir = process.argv[2];
const files = fs.readdirSync(dir).filter((name) => /^ppc_recomp\.\d+\.cpp$/.test(name));
for (const [name, lines] of edits) {
  const func = process.argv[3] ? `__imp__${process.argv[3]}` : name;
  const start = `PPC_FUNC_IMPL(${func}) {`;
  const file = files.find((name) => fs.readFileSync(path.join(dir, name), 'utf8').includes(start));
  if (!file) { console.error(`${func}: not found in ${dir}`); process.exit(1); }
  const text = fs.readFileSync(path.join(dir, file), 'utf8');
  const eol = text.includes('\r\n') ? '\r\n' : '\n';
  const all = text.split(eol);
  const first = all.indexOf(start);
  let end = first + 1;
  while (end < all.length && !all[end].startsWith('PPC_FUNC_IMPL(')) ++end;
  for (const [from, to] of lines) {
    const found = [];
    for (let i = first; i < end; ++i) if (all[i] === from) found.push(i);
    if (found.length !== 1) {
      console.error(`${file} ${func}: expected the line ${JSON.stringify(from)} once, found ${found.length}`);
      process.exit(1);
    }
    all[found[0]] = to;
  }
  fs.writeFileSync(path.join(dir, file), all.join(eol));
  console.log(`${file} ${func}: ${lines.length} edits applied`);
}
