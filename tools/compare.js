// Compares mdview's block structure with markdown-it's (what VS Code's preview uses).
//   npm i markdown-it   (anywhere resolvable), then:  node tools/compare.js mdview.exe file.md [more.md ...]
const { execFileSync } = require("child_process");
const fs = require("fs"), os = require("os"), path = require("path");
const md = require("markdown-it")({ html: true });

// markdown-it tokens -> the same one-token-per-block shorthand mdview /dump writes
function mdIt(src) {
  const out = [], toks = md.parse(src, {});
  let lists = 0, bq = 0;
  const item = [];                       // per open list item: has its first paragraph been absorbed yet
  for (const t of toks) {
    switch (t.type) {
      case "heading_open": out.push("H" + t.tag.slice(1)); break;
      case "bullet_list_open": case "ordered_list_open": lists++; break;
      case "bullet_list_close": case "ordered_list_close": lists--; break;
      case "list_item_open": out.push("L" + Math.min(lists - 1, 5)); item.push(false); break;
      case "list_item_close": item.pop(); break;
      case "blockquote_open": bq++; break;
      case "blockquote_close": bq--; break;
      case "paragraph_open":
        if (item.length && !item[item.length - 1]) item[item.length - 1] = true;   // the item's own text
        else out.push(bq && !item.length ? "Q" : "P");
        break;
      case "fence": case "code_block": out.push("C"); if (item.length) item[item.length - 1] = true; break;
      case "hr": out.push("R"); break;
      case "table_open": out.push("T"); break;
    }
  }
  // consecutive quote paragraphs are one block in mdview
  return out.filter((v, i) => !(v === "Q" && out[i - 1] === "Q"));
}

const exe = process.argv[2], files = process.argv.slice(3);
let bad = 0;
for (const f of files) {
  const tmp = path.join(os.tmpdir(), "mdview_dump.txt");
  try { fs.unlinkSync(tmp); } catch (e) {}
  execFileSync(exe, ["/dump", f, tmp]);
  const mine = fs.readFileSync(tmp, "utf8").split("\n").filter(Boolean).filter(x => x !== "D");
  const ref = mdIt(fs.readFileSync(f, "utf8"));
  let i = 0;
  while (i < mine.length && i < ref.length && mine[i] === ref[i]) i++;
  if (i < mine.length || i < ref.length) {
    bad++;
    console.log(`DIFF ${f}: block ${i}\n  mdview     ${mine.slice(Math.max(0, i - 2), i + 6).join(" ")}\n  markdown-it ${ref.slice(Math.max(0, i - 2), i + 6).join(" ")}`);
  }
}
console.log(`${files.length - bad}/${files.length} match`);
