const cellsEl = document.querySelector("#cells");
const template = document.querySelector("#cell-template");
const statusEl = document.querySelector("#status");
const addButton = document.querySelector("#add-cell");
const runAllButton = document.querySelector("#run-all");
const saveButton = document.querySelector("#save");
const interruptButton = document.querySelector("#interrupt");
const shutdownButton = document.querySelector("#shutdown");

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
  "fclose", "fread", "fwrite", "scanf", "nb_input", "zmq_ctx_new", "zmq_ctx_destroy",
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
let lastEventId = 0;
let activeRunIndex = null;
let pollTimer = null;
const pendingRuns = new Map();

function setStatus(message, isError = false) {
  statusEl.textContent = message;
  statusEl.classList.toggle("error", isError);
}

function defaultCell() {
  return {
    code: "int age;\nprintf(\"Enter your age: \");\nscanf(\"%d\", &age);\nprintf(\"age = %d\\n\", age);",
    output: "",
    executionCount: null
  };
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
  textarea.style.height = "auto";
  const nextHeight = Math.max(textarea.scrollHeight, 39);
  textarea.style.height = `${nextHeight}px`;
  highlight.style.height = textarea.style.height;
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

    textarea.addEventListener("input", () => {
      cell.code = textarea.value;
      syncHighlight(textarea, highlight);
    });

    textarea.addEventListener("scroll", () => {
      syncHighlight(textarea, highlight);
    });

    runButton.addEventListener("click", () => runCell(index).catch((error) => setStatus(error.message, true)));
    deleteButton.addEventListener("click", () => {
      const scrollY = window.scrollY;
      notebook.cells.splice(index, 1);
      if (notebook.cells.length === 0) {
        notebook.cells.push(defaultCell());
      }
      render();
      requestAnimationFrame(() => window.scrollTo(0, scrollY));
    });

    cellsEl.appendChild(node);
    syncHighlight(textarea, highlight);
  });
}

function payloadForRun(runIndex) {
  return {
    code: notebook.cells.slice(0, runIndex + 1).map((cell) => cell.code || "").join("\n"),
    silent: false,
    store_history: true,
    allow_stdin: true,
    run_index: runIndex,
    cells: notebook.cells.map((cell) => cell.code || "")
  };
}

async function loadNotebook() {
  const response = await fetch("/api/notebook");
  if (!response.ok) throw new Error("Failed to load notebook");
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
  if (!response.ok || !result.ok) throw new Error(result.error || "Save failed");
  setStatus("Saved.");
}

function assignExecutionCount(cell, count) {
  if (Number.isInteger(count)) {
    cell.executionCount = count;
    executionCounter = Math.max(executionCounter, count);
    return;
  }
  executionCounter += 1;
  cell.executionCount = executionCounter;
}

function appendCellOutput(index, text) {
  if (!notebook.cells[index]) return;
  notebook.cells[index].output = `${notebook.cells[index].output || ""}${text}`;
}

function handleIopub(event) {
  const content = event.content || {};
  if (event.msg_type === "status") {
    setStatus(`Kernel ${content.execution_state || "unknown"}.`);
    if (content.execution_state === "idle") activeRunIndex = null;
    return;
  }

  if (event.msg_type === "stream") {
    const index = Number.isInteger(content.cell_index) ? content.cell_index : activeRunIndex;
    appendCellOutput(index, content.text || "");
    const scrollY = window.scrollY;
    render();
    requestAnimationFrame(() => window.scrollTo(0, scrollY));
    return;
  }

  if (event.msg_type === "error") {
    const index = activeRunIndex ?? 0;
    appendCellOutput(index, `${content.evalue || "Execution error"}\n`);
    setStatus(content.evalue || "Execution error", true);
    const scrollY = window.scrollY;
    render();
    requestAnimationFrame(() => window.scrollTo(0, scrollY));
  }
}

function handleShell(event) {
  const content = event.content || {};
  if (event.msg_type !== "execute_reply") return;
  const runIndex = Number.isInteger(content.run_index) ? content.run_index : activeRunIndex;
  if (Number.isInteger(runIndex) && notebook.cells[runIndex]) {
    assignExecutionCount(notebook.cells[runIndex], content.execution_count);
    if (!notebook.cells[runIndex].output && Array.isArray(content.outputs)) {
      notebook.cells[runIndex].output = content.outputs[runIndex] || "";
    }
  }
  if (content.status === "error") {
    setStatus(content.evalue || "Execution failed", true);
  } else {
    setStatus(`Ran cell ${(runIndex ?? 0) + 1}.`);
  }
  const pending = pendingRuns.get(runIndex);
  if (pending) {
    pendingRuns.delete(runIndex);
    if (content.status === "error") {
      pending.reject(new Error(content.evalue || "Execution failed"));
    } else {
      pending.resolve(content);
    }
  }
  const scrollY = window.scrollY;
  render();
  requestAnimationFrame(() => window.scrollTo(0, scrollY));
}

async function handleStdin(event) {
  if (event.msg_type !== "input_request") return;
  const promptText = event.content?.prompt || "Input:";
  const value = window.prompt(promptText, "");
  await fetch("/api/stdin/reply", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ value: value ?? "" })
  });
}

async function pollEvents() {
  const response = await fetch(`/api/kernel/events?after=${lastEventId}`);
  if (!response.ok) return;
  const result = await response.json();
  if (!Array.isArray(result.events)) return;
  for (const event of result.events) {
    if (Number.isInteger(event.id)) lastEventId = Math.max(lastEventId, event.id);
    if (event.channel === "iopub") handleIopub(event);
    if (event.channel === "shell") handleShell(event);
    if (event.channel === "stdin") await handleStdin(event);
  }
}

function startPolling() {
  if (pollTimer) return;
  pollTimer = window.setInterval(() => {
    pollEvents().catch(() => {});
  }, 300);
}

async function runCell(index) {
  activeRunIndex = index;
  if (notebook.cells[index]) notebook.cells[index].output = "";
  render();
  setStatus(`Running cell ${index + 1}...`);
  const runReply = new Promise((resolve, reject) => {
    const timeout = window.setTimeout(() => {
      pendingRuns.delete(index);
      reject(new Error(`Timed out waiting for cell ${index + 1} reply`));
    }, 20000);
    pendingRuns.set(index, {
      resolve: (value) => {
        window.clearTimeout(timeout);
        resolve(value);
      },
      reject: (error) => {
        window.clearTimeout(timeout);
        reject(error);
      }
    });
  });
  const response = await fetch("/api/cell/run", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payloadForRun(index))
  });
  const result = await response.json();
  if (!response.ok || !result.ok) {
    pendingRuns.delete(index);
    throw new Error(result.error || "Failed to send execute_request");
  }
  startPolling();
  return runReply;
}

async function runAll() {
  for (let i = 0; i < notebook.cells.length; i++) {
    await runCell(i);
  }
}

async function postControl(path, message) {
  setStatus(message);
  const response = await fetch(path, { method: "POST" });
  const result = await response.json();
  if (!response.ok || !result.ok) throw new Error(result.error || message);
  startPolling();
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

interruptButton.addEventListener("click", () => {
  postControl("/api/kernel/interrupt", "Interrupt requested...").catch((error) => setStatus(error.message, true));
});

shutdownButton.addEventListener("click", () => {
  postControl("/api/kernel/shutdown", "Shutdown requested...").catch((error) => setStatus(error.message, true));
});

loadNotebook().catch((error) => {
  notebook.cells = [defaultCell()];
  render();
  setStatus(error.message, true);
});

startPolling();
