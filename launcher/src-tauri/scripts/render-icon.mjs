// Renders an SVG to a square transparent PNG for `tauri icon`, which wants a large PNG source.
// Usage: node render-icon.mjs <in.svg> <out.png> [size, default 1024]
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";
import { Resvg } from "@resvg/resvg-js";

const [input, output, sizeArg] = process.argv.slice(2);
if (!input || !output) {
  console.error("usage: node render-icon.mjs <in.svg> <out.png> [size]");
  process.exit(2);
}
const size = Number(sizeArg ?? 1024);
const svg = readFileSync(input);
const png = new Resvg(svg, {
  fitTo: { mode: "width", value: size },
  background: "rgba(0, 0, 0, 0)",
})
  .render()
  .asPng();
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, png);
console.log(`${output}: ${size}x${size} from ${input}`);
