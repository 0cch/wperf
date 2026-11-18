const fs = require("fs");
const os = require("os");
const path = require("path");
const vm = require("vm");
const { execFileSync } = require("child_process");

const wperf = process.argv[2];
if (!wperf) throw new Error("usage: flamegraph_browser_contract_test.js <wperf.exe>");
const directory = fs.mkdtempSync(path.join(os.tmpdir(), "wperf-flamegraph-js-"));

class Element {
  constructor(id = "") {
    this.id = id;
    this.textContent = "";
    this.value = "";
    this.style = {};
    this.handlers = {};
    this.children = [];
    this.parentElement = { clientWidth: 1200 };
    this.classList = { add() {}, remove() {} };
    this.viewBox = { baseVal: { width: 1200, height: 200 } };
    this.hidden = false;
    this.disabled = false;
  }
  addEventListener(type, fn) { this.handlers[type] = fn; }
  setAttribute(name, value) { this[name] = value; }
  appendChild(element) { this.children.push(element); return element; }
  replaceChildren(...nodes) { this.children = nodes; }
  querySelector() { return this.children.find(child => child.id === "range-overlay") || null; }
  remove() {}
  closest() { return this; }
  getBoundingClientRect() { return { left: 0, top: 0, width: 1200, height: 800 }; }
  setPointerCapture() {}
  hasPointerCapture() { return true; }
  releasePointerCapture() {}
}

function render(folded, name) {
  const input = path.join(directory, name + ".folded");
  const output = path.join(directory, name + ".html");
  fs.writeFileSync(input, folded, "utf8");
  execFileSync(wperf, ["flamegraph", "-i", input, "-o", output], { stdio: "pipe" });
  const html = fs.readFileSync(output, "utf8");
  const data = html.match(/<script type="application\/json" id="flame-data">([\s\S]*?)<\/script>/)[1];
  const scriptMatch = html.match(/<\/script>\s*<script>([\s\S]*?)<\/script>/);
  if (!scriptMatch) throw new Error("embedded flamegraph script is missing");
  const script = scriptMatch[1].replace(/\}\)\(\);\s*$/, "globalThis.__review={state,selectNode,zoomTo,selectionRoot};})();");
  const elements = new Map();
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new Element(id));
      return elements.get(id);
    },
    querySelectorAll() { return []; },
    createElementNS() { return new Element(); },
    createElement() { return new Element(); },
    createDocumentFragment() { return new Element(); },
    addEventListener() {},
    body: new Element()
  };
  document.getElementById("flame-data").textContent = data;
  const copied = [];
  const sandbox = {
    document,
    window: { addEventListener() {}, innerWidth: 1200 },
    navigator: { clipboard: { async writeText(text) { copied.push(text); } } },
    Intl, Math, JSON, Number, BigInt, performance, setTimeout() {}, URL, Blob
  };
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox);
  return { elements, copied, review: sandbox.__review };
}

function check(condition, message) { if (!condition) throw new Error(message); }

try {
  const normal = render(
    "app.exe[42@1];main;alpha 60\napp.exe[42@1];main;beta 40\n",
    "normal");
  const main = normal.review.state.frameAreas.find(area => area.node.name === "main");
  const alpha = normal.review.state.frameAreas.find(area => area.node.name === "alpha");
  check(main && alpha, "normal flamegraph frames are missing");
  normal.review.selectNode(alpha.node, alpha.path, alpha.total, alpha.rect);
  normal.review.zoomTo(main.node, main.path);
  const zoomedAlpha = normal.review.state.frameAreas.find(area => area.node.name === "alpha");
  check(zoomedAlpha, "zoomed flamegraph lost the child frame");
  normal.review.selectNode(zoomedAlpha.node, zoomedAlpha.path, zoomedAlpha.total, zoomedAlpha.rect);
  const stack = normal.elements.get("details-stack").textContent;
  check(stack.includes("app.exe[42@1]") && stack.includes("main") && stack.includes("alpha"),
        "zoomed stack lost canonical ancestors");
  normal.elements.get("copy-folded").handlers.click();
  const copied = normal.copied[normal.copied.length - 1];
  check(copied === "root;app.exe[42@1];main;alpha 60",
        "zoomed folded copy is not canonical: " + copied);

  const maximum = "18446744073709551615";
  const max = render("app;main " + maximum + "\n", "maximum");
  const maxMain = max.review.state.frameAreas.find(area => area.node.name === "main");
  check(maxMain, "maximum-weight frame is missing");
  max.review.selectNode(maxMain.node, maxMain.path, maxMain.total, maxMain.rect);
  max.elements.get("copy-folded").handlers.click();
  check(max.copied[max.copied.length - 1] === "root;app;main " + maximum,
        "uint64 weight was not preserved exactly");
  const fullRange = max.review.selectionRoot(max.review.state.focus, 0, 1);
  check(fullRange.valueText === maximum,
        "full-range weight is not clamped to uint64");
  check(fullRange.name.includes("approx"),
        "range-selection node is not marked approximate");
  console.log("flamegraph browser contract: PASS");
} finally {
  fs.rmSync(directory, { recursive: true, force: true });
}
