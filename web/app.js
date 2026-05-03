const cellsEl = document.querySelector("#cells");
const template = document.querySelector("#cell-template");
const statusEl = document.querySelector("#status");
const addButton = document.querySelector("#add-cell");
const runAllButton = document.querySelector("#run-all");
const saveButton = document.querySelector("#save");

const cKeywords = new Set([
  "auto", "break", "case", "const", "continue", "default", "do", "else",
  "enum", "extern", "for", "goto", "if", "register", "return", "sizeof",
  "static", "struct", "switch", "typedef", "union", "volatile", "while"
]);

const cTypes = new Set([
  "char", "double", "float", "int", "long", "short", "signed", "unsigned",
  "void", "size_t", "FILE", "pthread_t", "zmq_msg_t"
]);

const cFunctions = new Set([
  "printf", "fprintf", "snprintf", "malloc", "calloc", "realloc", "free",
  "memcpy", "memset", "strlen", "strcmp", "strdup", "system", "fopen",
  "fclose", "fread", "fwrite", "zmq_ctx_new", "zmq_ctx_destroy",
  "zmq_socket", "zmq_close", "zmq_setsockopt", "zmq_getsockopt",
  "zmq_bind", "zmq_connect", "zmq_send", "zmq_recv", "zmq_msg_init",
  "zmq_msg_init_size", "zmq_msg_init_data", "zmq_msg_data",
  "zmq_msg_size", "zmq_msg_send", "zmq_msg_recv", "zmq_msg_close",
  "zmq_poll", "zmq_proxy", "zmq_errno", "zmq_strerror"
]);

let notebook = {
  title: "ZeroMQ Notebook Clone",
  cells: []
};

let executionCounter = 0;

function setStatus(message, isError = false) {
  statusEl.textContent = message;
  statusEl.classList.toggle("error", isError);
}

function defaultCell() {
  return { code: "printf(\"hello zeromq notebook\\n\");", output: "", executionCount: null };
}

function escapeHtml(text) {
  return text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function span(className, text) {
  return `<span class="${className}">${escapeHtml(text)}</span>`;
}

function highlightC(code) {
  const tokenPattern = /("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\/\/[^\n]*|\/\*[\s\S]*?\*\/|^\s*#\s*[a-zA-Z_]\w*|\b\d+(?:\.\d+)?\b|\b[A-Za-z_]\w*\b)/gm;
  return code.replace(tokenPattern, (token) => {
    const trimmed = token.trimStart();
    if (trimmed.startsWith("#")) return span("tok-preprocessor", token);
    if (token.startsWith("//") || token.startsWith("/*")) return span("tok-comment", token);
    if (token.startsWith("\"") || token.startsWith("'")) return span("tok-string", token);
    if (/^\d/.test(token)) return span("tok-number", token);
    if (cKeywords.has(token)) return span("tok-keyword", token);
    if (cTypes.has(token)) return span("tok-type", token);
    if (cFunctions.has(token)) return span("tok-function", token);
    return escapeHtml(token);
  });
}

function syncHighlight(textarea, highlight) {
  highlight.innerHTML = `${highlightC(textarea.value)}\n`;
  highlight.scrollTop = textarea.scrollTop;
  highlight.scrollLeft = textarea.scrollLeft;
}

function render() {
  cellsEl.textContent = "";
  notebook.cells.forEach((cell, index) => {
    const node = template.content.firstElementChild.cloneNode(true);
    const label = node.querySelector(".cell-label");
    const inputPrompt = node.querySelector(".input-prompt");
    const outputPrompt = node.querySelector(".output-prompt");
    const textarea = node.querySelector("textarea");
    const highlight = node.querySelector(".syntax-highlight");
    const output = node.querySelector(".output");
    const runButton = node.querySelector(".run-cell");
    const deleteButton = node.querySelector(".delete-cell");

    const count = Number.isInteger(cell.executionCount) ? cell.executionCount : " ";
    label.textContent = "Code";
    inputPrompt.textContent = `In [${count}]:`;
    outputPrompt.textContent = cell.output ? `Out[${count}]:` : "";
    textarea.value = cell.code || "";
    output.textContent = cell.output || "";
    syncHighlight(textarea, highlight);

    textarea.addEventListener("input", () => {
      cell.code = textarea.value;
      syncHighlight(textarea, highlight);
    });

    textarea.addEventListener("scroll", () => {
      syncHighlight(textarea, highlight);
    });

    runButton.addEventListener("click", () => runCell(index));
    deleteButton.addEventListener("click", () => {
      notebook.cells.splice(index, 1);
      if (notebook.cells.length === 0) {
        notebook.cells.push(defaultCell());
      }
      render();
    });

    cellsEl.appendChild(node);
  });
}

function payloadForRun(runIndex) {
  return {
    run_index: runIndex,
    cells: notebook.cells.map((cell) => cell.code || "")
  };
}

async function loadNotebook() {
  const response = await fetch("/api/notebook");
  if (!response.ok) {
    throw new Error("Failed to load notebook");
  }
  notebook = await response.json();
  if (!Array.isArray(notebook.cells) || notebook.cells.length === 0) {
    notebook.cells = [defaultCell()];
  }
  notebook.cells = notebook.cells.map((cell) => ({
    code: typeof cell.code === "string" ? cell.code : "",
    output: typeof cell.output === "string" ? cell.output : "",
    executionCount: Number.isInteger(cell.executionCount) ? cell.executionCount : null
  }));
  executionCounter = notebook.cells.reduce((max, cell) => {
    return Number.isInteger(cell.executionCount) ? Math.max(max, cell.executionCount) : max;
  }, 0);
  render();
  setStatus("Notebook loaded.");
}

async function saveNotebook() {
  setStatus("Saving...");
  const response = await fetch("/api/notebook/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(notebook)
  });
  const result = await response.json();
  if (!response.ok || !result.ok) {
    throw new Error(result.error || "Save failed");
  }
  setStatus("Saved.");
}

function applyRunResult(result, runIndex) {
  if (Array.isArray(result.outputs)) {
    result.outputs.forEach((output, index) => {
      if (notebook.cells[index]) {
        notebook.cells[index].output = output;
        notebook.cells[index].executionCount = executionCounter + index + 1;
      }
    });
    executionCounter += result.outputs.length;
  }

  if (!result.ok) {
    const target = Number.isInteger(result.run_index) ? result.run_index : runIndex;
    if (notebook.cells[target]) {
      notebook.cells[target].output = result.error || "Execution failed";
    }
    setStatus(result.error || "Execution failed", true);
  } else {
    setStatus(`Ran through cell ${runIndex + 1}.`);
  }
  render();
}

async function runCell(index) {
  setStatus(`Running cell ${index + 1}...`);
  const response = await fetch("/api/cell/run", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payloadForRun(index))
  });
  const result = await response.json();
  applyRunResult(result, index);
}

async function runAll() {
  const index = notebook.cells.length - 1;
  setStatus("Running all cells...");
  const response = await fetch("/api/run-all", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payloadForRun(index))
  });
  const result = await response.json();
  applyRunResult(result, index);
}

addButton.addEventListener("click", () => {
  notebook.cells.push(defaultCell());
  render();
});

saveButton.addEventListener("click", () => {
  saveNotebook().catch((error) => setStatus(error.message, true));
});

runAllButton.addEventListener("click", () => {
  runAll().catch((error) => setStatus(error.message, true));
});

loadNotebook().catch((error) => {
  notebook.cells = [defaultCell()];
  render();
  setStatus(error.message, true);
});
