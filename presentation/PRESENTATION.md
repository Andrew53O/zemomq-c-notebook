# Presentation Material: Mini Jupyter Notebook Clone Using C + ZeroMQ

Target length: 15-20 minutes

Recommended slide style: use mostly screenshots, architecture diagrams, and short keywords on slides. Use this file as speaker notes.

---

## 0. Presentation Goal

Slide content:

- Mini Jupyter Notebook Clone
- Built with C + ZeroMQ
- Run C snippets from a browser

Visual suggestion:

- Show a screenshot of the notebook UI.
- Show the simple architecture diagram from README.

English speaker notes:

Today we are presenting a small project inspired by Jupyter Notebook. The goal is not to rebuild all of Jupyter, but to demonstrate how ZeroMQ can connect multiple processes in a real application. From the browser, the user writes C code and clicks Run. Behind that simple action, the system uses an HTTP server, a ZeroMQ broker, and a C kernel worker to compile and execute the code.

The important point is that ZeroMQ is not just used as a small library call. It is part of the architecture. We use ZeroMQ socket types, message patterns, multipart messages, proxying, polling, and status events to make this notebook-like workflow possible.

Chinese speaker notes:

今天我們介紹的是一個簡化版的 Jupyter Notebook clone。目標不是完整重做 Jupyter，而是用一個實際專案展示 ZeroMQ 怎麼把多個 process 串起來。使用者在瀏覽器裡寫 C code，按下 Run，看起來只是一個簡單按鈕，但背後其實經過 C HTTP server、ZeroMQ broker，最後到 C kernel worker 編譯和執行程式。

重點是 ZeroMQ 不是只有拿來 send/receive 一個字串，而是成為整個系統架構的一部分。我們會展示 socket type、message pattern、multipart message、proxy、polling、status event 等概念。

---

## 1. What Happens When We Click Run?

Slide content:

```text
Browser UI
  -> C HTTP Server
  -> ZeroMQ ROUTER/DEALER Broker
  -> C Kernel Worker
  -> Generated C Program
  -> Output returns to Browser
```

Visual suggestion:

- Use the Manim video first.
- Pause after each node appears.

English speaker notes:

We start from the user's perspective. The user types a C snippet into the notebook cell, for example `printf("hello\n");`, and clicks Run. The browser itself does not compile or run code. It only collects the notebook cells and sends them to the local C HTTP server.

The HTTP server receives the request through `/api/cell/run` or `/api/run-all`. Then it creates a ZeroMQ request socket and sends the execution request to the ZeroMQ broker. The broker forwards the request to an available worker. The worker generates a complete C program, compiles it with `gcc`, runs it with a timeout, captures stdout or compiler errors, and sends the result back through the same path.

This is the main story of the project: a simple notebook UI, but a distributed internal architecture.

Chinese speaker notes:

我們先從使用者角度看。使用者在 notebook cell 裡輸入 C snippet，例如 `printf("hello\n");`，然後按 Run。瀏覽器本身不會編譯或執行 C code，它只負責把目前 notebook 的 cell 內容送給本機的 C HTTP server。

HTTP server 透過 `/api/cell/run` 或 `/api/run-all` 收到 request。接著 server 建立 ZeroMQ request socket，把執行要求送到 ZeroMQ broker。Broker 再把 request 轉給一個可用的 worker。Worker 會產生完整的 C 程式，用 `gcc` 編譯，使用 timeout 執行，捕捉 stdout 或 compiler error，最後再沿著同一條路徑把結果送回瀏覽器。

這就是整個專案的主軸：畫面看起來像簡單 notebook，但內部是多 process 的架構。

---

## 2. Browser UI: Notebook Experience

Slide content:

- Cells
- `In [ ]:` and `Out [ ]:`
- Run cell / Run all
- C syntax highlighting

Visual suggestion:

- Show the UI.
- Highlight one input cell and one output cell.

English speaker notes:

The frontend is intentionally designed to look similar to a classic Jupyter Notebook. It has input prompts like `In [ ]:` and output prompts like `Out [ ]:`. The code editor supports lightweight C syntax highlighting, so keywords, strings, comments, numbers, and ZeroMQ function names are easier to read.

However, the frontend is still only a static HTML, CSS, and JavaScript interface. It does not execute the C code. JavaScript collects the cells, sends JSON to the C server, receives the output, and updates the UI.

This separation is important. The browser is the presentation layer. The execution layer is handled by C processes behind ZeroMQ.

Chinese speaker notes:

前端刻意設計得接近經典 Jupyter Notebook。它有 `In [ ]:` 和 `Out [ ]:`，也有 Run cell 和 Run all。編輯器加入簡單的 C syntax highlighting，所以 keyword、string、comment、number，以及 ZeroMQ function name 都會有顏色，比較容易閱讀。

但是前端只是 static HTML、CSS、JavaScript。它不會執行 C code。JavaScript 只負責收集 cell，把 JSON 送給 C server，收到 output 之後更新畫面。

這個分工很重要。Browser 是 presentation layer，真正的執行層是在後面的 C process 和 ZeroMQ 架構。

---

## 3. C HTTP Server: Bridge from Web to ZeroMQ

Slide content:

- Serves web files
- Receives HTTP API calls
- Creates `ZMQ_REQ` socket
- Sends multipart ZeroMQ request

Visual suggestion:

- Show Browser -> Server arrow.
- Show file: `src/server.c`.

English speaker notes:

The C HTTP server has two responsibilities. First, it serves the static web files: `index.html`, `styles.css`, and `app.js`. Second, it receives API calls from the browser.

When a run request arrives, the server creates a ZeroMQ socket using `zmq_socket(context, ZMQ_REQ)`. This is the request side of the request-reply pattern. The server connects to the broker frontend at `tcp://127.0.0.1:7000`.

The server does not call the worker directly. This is important. It sends the request into the ZeroMQ topology, so the broker can decide which worker receives it.

Chinese speaker notes:

C HTTP server 有兩個責任。第一，它負責提供 static web files，也就是 `index.html`、`styles.css`、`app.js`。第二，它接收 browser 發出的 API request。

當 Run request 進來時，server 會用 `zmq_socket(context, ZMQ_REQ)` 建立 ZeroMQ socket。這是 request-reply pattern 裡的 request 端。Server connect 到 broker frontend，也就是 `tcp://127.0.0.1:7000`。

Server 不直接呼叫 worker。這點很重要。它把 request 丟進 ZeroMQ topology，讓 broker 負責決定哪個 worker 來處理。

---

## 4. Socket API: Four Life Parts

Slide content:

- Create / destroy
- Configure
- Bind / connect
- Send / receive

Visual suggestion:

- Use a four-step diagram.
- Put function names around the diagram.

English speaker notes:

This project uses the four main parts of the ZeroMQ socket API.

First, create and destroy. We create a context with `zmq_ctx_new`, create sockets with `zmq_socket`, close sockets with `zmq_close`, and destroy the context with `zmq_ctx_destroy`.

Second, configure. We use `zmq_setsockopt` for options such as `ZMQ_LINGER`, send and receive timeouts, subscriptions, and high-water marks. The broker also uses `zmq_getsockopt` to read back the actual high-water mark.

Third, plug sockets into the topology. The broker uses `zmq_bind` because it owns stable endpoints. The server and workers use `zmq_connect` because they join the topology.

Fourth, carry data. We use both simple APIs like `zmq_send` and `zmq_recv`, and message APIs like `zmq_msg_send` and `zmq_msg_recv`.

Chinese speaker notes:

這個專案使用 ZeroMQ socket API 的四個主要生命週期。

第一，create 和 destroy。我們用 `zmq_ctx_new` 建立 context，用 `zmq_socket` 建立 socket，用 `zmq_close` 關閉 socket，最後用 `zmq_ctx_destroy` 釋放 context。

第二，configure。我們用 `zmq_setsockopt` 設定 `ZMQ_LINGER`、send/receive timeout、subscription、high-water mark。Broker 也用 `zmq_getsockopt` 讀回實際的 high-water mark。

第三，plug into topology。Broker 用 `zmq_bind`，因為它提供穩定 endpoint。Server 和 worker 用 `zmq_connect`，因為它們是加入這個 topology 的節點。

第四，carry data。我們同時使用簡單 API，例如 `zmq_send`、`zmq_recv`，也使用 message API，例如 `zmq_msg_send`、`zmq_msg_recv`。

---

## 5. Bind and Connect: Server Side vs Client Side

Slide content:

```text
Broker binds:
  tcp://127.0.0.1:7000
  tcp://127.0.0.1:7001

Server and workers connect.
```

Visual suggestion:

- Show broker in the middle.
- Label broker as stable address owner.

English speaker notes:

In ZeroMQ, the node that binds is usually the stable side. In this project, the broker binds two endpoints. The frontend endpoint accepts requests from the HTTP server. The backend endpoint accepts workers.

The HTTP server connects to the frontend. Each kernel worker connects to the backend. This means we can start a new worker without changing the browser or server. The worker simply connects to the broker, and it becomes available.

This demonstrates the topology idea: bind creates an address, connect joins that address.

Chinese speaker notes:

在 ZeroMQ 裡，bind 的節點通常是比較穩定的那一端。這個專案中，broker bind 兩個 endpoint。Frontend endpoint 接收 HTTP server 的 request；backend endpoint 接收 worker。

HTTP server connect 到 frontend。每個 kernel worker connect 到 backend。這代表我們可以啟動新的 worker，而不用改 browser 或 server。Worker 只要 connect 到 broker，就能加入系統。

這展示了 topology 的概念：bind 建立地址，connect 加入地址。

---

## 6. REQ/REP: The Execution Request Pattern

Slide content:

- Server: `ZMQ_REQ`
- Worker: `ZMQ_REP`
- Must follow send/receive order

Visual suggestion:

- Show request arrow and reply arrow.

English speaker notes:

The main execution path uses request-reply behavior. The HTTP server creates a `ZMQ_REQ` socket. The worker creates a `ZMQ_REP` socket.

This is where we can see that ZeroMQ is not a neutral carrier. A `REQ` socket is not just any socket. It enforces a behavior: send first, then receive. A `REP` socket must receive first, then send. If we break that order, the socket pattern is broken.

So socket type is part of the protocol design. It tells us what communication behavior is allowed.

Chinese speaker notes:

主要執行路徑使用 request-reply 行為。HTTP server 建立 `ZMQ_REQ` socket，worker 建立 `ZMQ_REP` socket。

這裡可以看到 ZeroMQ 不是 neutral carrier。`REQ` socket 不是普通 socket，它規定行為：先 send，再 receive。`REP` socket 則是先 receive，再 send。如果順序錯了，pattern 就會壞掉。

所以 socket type 是 protocol design 的一部分。它不只傳資料，也定義通訊行為。

---

## 7. ROUTER/DEALER Broker: Shared Queue

Slide content:

- Frontend: `ZMQ_ROUTER`
- Backend: `ZMQ_DEALER`
- `zmq_proxy(frontend, backend, NULL)`
- Supports multiple workers

Visual suggestion:

- Show one server, one broker, multiple workers.

English speaker notes:

The broker is the core ZeroMQ architecture component. It creates a `ZMQ_ROUTER` socket for the frontend and a `ZMQ_DEALER` socket for the backend.

The frontend talks to request clients. The backend talks to workers. Instead of manually forwarding every frame, the broker uses `zmq_proxy(frontend, backend, NULL)`. This is ZeroMQ's built-in proxy function.

This solves the dynamic discovery problem. The server does not need to know how many workers exist. Workers can start later, connect to the backend, and begin receiving work. This is a shared queue pattern.

Chinese speaker notes:

Broker 是這個系統最核心的 ZeroMQ 架構元件。它建立一個 `ZMQ_ROUTER` 作為 frontend，再建立一個 `ZMQ_DEALER` 作為 backend。

Frontend 跟 request client 溝通，backend 跟 worker 溝通。Broker 不需要手動轉發每一個 frame，而是使用 `zmq_proxy(frontend, backend, NULL)`，也就是 ZeroMQ 內建 proxy function。

這解決 dynamic discovery problem。Server 不需要知道有幾個 worker。Worker 可以晚一點啟動，只要 connect 到 backend，就能開始接收工作。這就是 shared queue pattern。

---

## 8. Multipart Messages and Message API

Slide content:

```text
Request:
  frame 1: RUN
  frame 2: JSON payload

Reply:
  frame 1: RESULT
  frame 2: JSON output
```

Visual suggestion:

- Draw stacked message frames.

English speaker notes:

The server sends execution requests as multipart messages. The first frame is a command, `RUN`. The second frame is the JSON payload containing the cells and the target cell index.

The worker replies with another multipart message. The first frame is `RESULT`, and the second frame is the JSON result.

This demonstrates ZeroMQ message frames. In the code, we use `zmq_msg_init_size`, `zmq_msg_data`, `zmq_msg_send`, `zmq_msg_recv`, and `zmq_msg_close`. We also use `ZMQ_SNDMORE` when sending all frames except the last one, and `ZMQ_RCVMORE` to check if more frames are waiting.

Chinese speaker notes:

Server 送 execution request 時使用 multipart message。第一個 frame 是 command，也就是 `RUN`。第二個 frame 是 JSON payload，裡面包含所有 cells 和要執行到第幾個 cell。

Worker 回覆時也使用 multipart message。第一個 frame 是 `RESULT`，第二個 frame 是 JSON result。

這展示 ZeroMQ message frame 的概念。在程式中，我們使用 `zmq_msg_init_size`、`zmq_msg_data`、`zmq_msg_send`、`zmq_msg_recv`、`zmq_msg_close`。送多個 frame 時，除了最後一個 frame，前面的 frame 都會使用 `ZMQ_SNDMORE`；接收時用 `ZMQ_RCVMORE` 確認後面還有沒有 frame。

---

## 9. C Kernel Worker: Cumulative Notebook Execution

Slide content:

- Receive JSON cells
- Generate one C program
- Compile with `gcc`
- Run with timeout
- Split output by cell markers

Visual suggestion:

- Show code cell 1: `int x = 42;`
- Show code cell 2: `printf("%d\n", x);`

English speaker notes:

The worker is what makes the project feel like a notebook. It receives all cells up to the selected cell. Then it generates one C source file with standard includes and an `int main(void)` function.

Each notebook cell is inserted into the generated program in order. That is why the notebook has cumulative behavior. If cell 1 defines `int x = 42;`, cell 2 can use `x`, because both snippets are compiled into the same generated C program.

The worker adds markers like `__CELL_START_0__` and `__CELL_END_0__` around each cell. After running the binary, it uses those markers to split stdout back into per-cell outputs.

Chinese speaker notes:

Worker 是讓這個專案像 notebook 的關鍵。它會收到從第一個 cell 到目前要執行的 cell 的所有內容。接著它產生一個 C source file，包含 standard includes 和 `int main(void)`。

每個 notebook cell 都會依序放進這個 generated program。這就是為什麼它有 cumulative behavior。如果 cell 1 定義 `int x = 42;`，cell 2 可以使用 `x`，因為兩段 code 會被編譯進同一個 C program。

Worker 會在每個 cell 前後加入 marker，例如 `__CELL_START_0__` 和 `__CELL_END_0__`。執行 binary 之後，再用這些 marker 把 stdout 分回每個 cell 的 output。

---

## 10. Handling Multiple Sockets with `zmq_poll`

Slide content:

- Worker uses `zmq_poll`
- Server drains status events with `zmq_poll`
- Avoids blocking forever

Visual suggestion:

- Show worker waiting for events.

English speaker notes:

ZeroMQ applications often need to handle more than one possible event source. This project uses `zmq_poll` to make long-running processes more responsive.

The worker polls its `ZMQ_REP` socket before receiving execution requests. This lets it periodically check whether it should continue running. The server also uses `zmq_poll` when draining the status channel so it can process available status messages without blocking.

This is a basic version of an event loop.

Chinese speaker notes:

ZeroMQ application 常常需要處理不只一個事件來源。這個專案使用 `zmq_poll`，讓長時間執行的 process 比較容易控制。

Worker 在接收 execution request 前會 poll 它的 `ZMQ_REP` socket。這讓它可以定期檢查是否要繼續執行。Server 在讀 status channel 時也使用 `zmq_poll`，所以它可以讀取目前可用的 status message，而不會卡住。

這是一個基本 event loop 的概念。

---

## 11. PUB/SUB Status Channel and Message Envelopes

Slide content:

- Worker publishes status
- Topic: `kernel`
- Server subscribes to `kernel`

Visual suggestion:

- Show topic envelope: `[kernel] [run-start]`

English speaker notes:

The project also includes a status channel. The worker creates a `ZMQ_PUB` socket and publishes events such as `worker-ready`, `run-start`, and `run-finished`.

The server creates a `ZMQ_SUB` socket and subscribes to the topic `kernel`. This shows the PUB/SUB envelope idea. The first frame is the topic, and the subscriber decides which topics it wants to receive.

This is useful because status messages are not part of the main REQ/REP execution path. They are asynchronous side information.

Chinese speaker notes:

這個專案也有 status channel。Worker 建立 `ZMQ_PUB` socket，發布例如 `worker-ready`、`run-start`、`run-finished` 的事件。

Server 建立 `ZMQ_SUB` socket，並訂閱 `kernel` topic。這展示 PUB/SUB envelope 的概念。第一個 frame 是 topic，subscriber 決定自己要接收哪些 topic。

這很有用，因為 status message 不是主要 REQ/REP 執行路徑的一部分，而是非同步的輔助資訊。

---

## 12. High-Water Marks and Socket Options

Slide content:

- `ZMQ_SNDHWM`
- `ZMQ_RCVHWM`
- Prevent unbounded queues

Visual suggestion:

- Show queue with max size.

English speaker notes:

ZeroMQ sockets have internal queues. If messages are produced faster than they are consumed, those queues can grow. High-water marks limit the queue size.

In the broker, we set `ZMQ_SNDHWM` and `ZMQ_RCVHWM` on both frontend and backend sockets. We also read back the value with `zmq_getsockopt`. This demonstrates both setting and checking socket options.

This is not a complete reliability mechanism, but it is important for memory control and back-pressure.

Chinese speaker notes:

ZeroMQ socket 內部有 queue。如果 message 產生速度比消耗速度快，queue 可能會越來越大。High-water mark 可以限制 queue 大小。

在 broker 裡，我們對 frontend 和 backend socket 設定 `ZMQ_SNDHWM` 和 `ZMQ_RCVHWM`。我們也用 `zmq_getsockopt` 讀回設定值。這展示了設定和檢查 socket option。

這不是完整的可靠性機制，但對 memory control 和 back-pressure 很重要。

---

## 13. PAIR and Thread Signaling Demo

Slide content:

- `ZMQ_PAIR`
- `inproc://`
- One socket per thread

Demo command:

```bash
./build/pair_signal_demo
```

English speaker notes:

ZeroMQ sockets should not be shared across threads. The demo `pair_signal_demo` shows a safer pattern. The main thread creates one `ZMQ_PAIR` socket, and the worker thread creates another `ZMQ_PAIR` socket. They communicate through `inproc://node-control`.

The main thread sends `start-node`, and the worker replies `worker-ack`. This demonstrates thread signaling and node coordination.

Chinese speaker notes:

ZeroMQ socket 不應該被多個 thread 共享。`pair_signal_demo` 展示比較安全的做法。Main thread 建立一個 `ZMQ_PAIR` socket，worker thread 建立另一個 `ZMQ_PAIR` socket，兩者透過 `inproc://node-control` 溝通。

Main thread 送 `start-node`，worker 回覆 `worker-ack`。這展示 thread signaling 和 node coordination。

---

## 14. Zero-Copy Message Demo

Slide content:

- `zmq_msg_init_data`
- Fourth argument: free callback
- ZeroMQ owns buffer until callback

Demo command:

```bash
./build/zero_copy_demo
```

English speaker notes:

The zero-copy demo shows `zmq_msg_init_data`. Instead of copying data into a ZeroMQ message, we give ZeroMQ a pointer to an existing buffer.

The important detail is the fourth argument: the free callback. ZeroMQ calls this callback when it is finished with the buffer. That means the application must not free or modify the buffer too early.

This connects directly to the Working with Messages section: message ownership matters.

Chinese speaker notes:

Zero-copy demo 展示 `zmq_msg_init_data`。它不是把資料 copy 進 ZeroMQ message，而是把現有 buffer 的 pointer 交給 ZeroMQ。

重要的是第四個參數：free callback。ZeroMQ 用完 buffer 後會呼叫這個 callback。這代表 application 不能太早 free 或修改這個 buffer。

這跟 Working with Messages 章節直接相關：message ownership 很重要。

---

## 15. Transport Bridging Demo

Slide content:

- `tcp://`
- `ipc://`
- `zmq_proxy`

Demo command:

```bash
./build/transport_bridge_demo
```

English speaker notes:

The main notebook uses `tcp://` because it is simple to demonstrate locally. We also include a transport bridge demo that binds one side to TCP and another side to IPC.

This shows that ZeroMQ can bridge different transports. The application logic can stay similar while the transport changes. The bridge again uses `zmq_proxy`, showing that proxying is a general ZeroMQ tool.

Chinese speaker notes:

主要 notebook 使用 `tcp://`，因為本機 demo 很容易。除此之外，我們也加入 transport bridge demo，一邊 bind 到 TCP，另一邊 bind 到 IPC。

這展示 ZeroMQ 可以 bridge 不同 transport。Application logic 可以維持類似，但底層 transport 可以改變。這個 bridge 也使用 `zmq_proxy`，表示 proxy 是 ZeroMQ 很通用的工具。

---

## 16. Error Handling and Interrupt Handling

Slide content:

- `zmq_errno`
- `zmq_strerror`
- `SIGINT`
- `EINTR`
- timeout

Visual suggestion:

- Show compile error and infinite loop timeout.

English speaker notes:

Because this project runs user-written C snippets, failures are expected. The worker handles compiler errors and runtime timeouts. For example, if the user forgets a semicolon, the compiler error is returned to the browser. If the user writes `while (1) {}`, the worker runs it with the `timeout` command and returns an execution timeout.

For ZeroMQ errors, the code uses `zmq_errno` and `zmq_strerror` to print meaningful messages. Long-running processes also handle `SIGINT`, `SIGTERM`, and `EINTR`, so Ctrl-C shutdown is cleaner.

Chinese speaker notes:

因為這個專案會執行使用者寫的 C snippet，所以失敗是正常情況。Worker 會處理 compiler error 和 runtime timeout。例如忘記分號時，compiler error 會回到 browser。如果使用者寫 `while (1) {}`，worker 會用 `timeout` 執行，最後回傳 execution timeout。

ZeroMQ error 方面，程式使用 `zmq_errno` 和 `zmq_strerror` 印出有意義的錯誤訊息。長時間執行的 process 也處理 `SIGINT`、`SIGTERM`、`EINTR`，所以 Ctrl-C shutdown 比較乾淨。

---

## 17. Suggested Live Demo Flow

Slide content:

- Start broker
- Start worker
- Start server
- Open browser
- Run cells

Commands:

```bash
./build/broker
./build/kernel_worker
./build/server
```

English speaker notes:

For the live demo, start three terminals. First, start the broker. Second, start the kernel worker. Third, start the server. Then open `http://127.0.0.1:8080`.

Run the first cell that defines `int x = 42;`. Then run the second cell that prints `x`. This proves cumulative execution. Next, show a compile error by missing a semicolon. Then show timeout with `while (1) {}`.

If time allows, start another worker to show dynamic discovery. The browser and server do not need to change. The new worker simply connects to the broker.

Chinese speaker notes:

Live demo 建議開三個 terminal。第一個啟動 broker，第二個啟動 kernel worker，第三個啟動 server。然後打開 `http://127.0.0.1:8080`。

先執行第一個 cell，定義 `int x = 42;`。再執行第二個 cell 印出 `x`，證明 cumulative execution。接著示範忘記分號的 compile error，再示範 `while (1) {}` 的 timeout。

如果時間夠，可以再開一個 worker，展示 dynamic discovery。Browser 和 server 不需要改，新的 worker 只要 connect 到 broker 就可以加入。

---

## 18. What This Project Teaches

Slide content:

- ZeroMQ socket types define behavior
- Broker solves dynamic discovery
- Multipart messages structure data
- Workers scale independently
- Notebook UI hides distributed internals

English speaker notes:

This project shows that ZeroMQ is useful when we want to connect independent processes with clear messaging patterns. The browser sees a notebook. Internally, the system has a server, broker, workers, generated programs, and status channels.

The biggest ZeroMQ lesson is that socket types matter. `REQ`, `REP`, `ROUTER`, `DEALER`, `PUB`, `SUB`, and `PAIR` are not just transport choices. They define how nodes communicate.

So this project turns the chapter concepts into a working system: socket lifecycle, topology, message patterns, multipart messages, proxy, dynamic discovery, polling, thread signaling, transport bridging, and error handling.

Chinese speaker notes:

這個專案展示 ZeroMQ 適合用在多個獨立 process 之間的溝通，而且每個溝通模式都很清楚。使用者看到的是 notebook，但內部其實有 server、broker、worker、generated program、status channel。

最大的 ZeroMQ 重點是 socket type 很重要。`REQ`、`REP`、`ROUTER`、`DEALER`、`PUB`、`SUB`、`PAIR` 不只是傳輸方式的選擇，它們定義節點之間怎麼溝通。

所以這個專案把章節概念變成實際系統：socket lifecycle、topology、message pattern、multipart message、proxy、dynamic discovery、polling、thread signaling、transport bridging、error handling。

---

## 19. Closing

Slide content:

- A notebook is simple on screen
- ZeroMQ coordinates the backend
- C code runs in a separate worker process

English speaker notes:

To conclude, this project starts from a familiar interface: a notebook. But when we click Run, we can see a full ZeroMQ-based message flow behind it. The UI sends a request, the server translates it into a ZeroMQ message, the broker routes it, the worker compiles and runs C code, and the result comes back to the browser.

This makes the ZeroMQ concepts easier to understand because each concept has a role in the system, not just in a small isolated example.

Chinese speaker notes:

總結來說，這個專案從大家熟悉的 notebook 介面開始。但當我們按下 Run，就能看到背後完整的 ZeroMQ message flow。UI 送出 request，server 把它轉成 ZeroMQ message，broker 負責 routing，worker 編譯和執行 C code，最後結果回到 browser。

這讓 ZeroMQ 的概念比較容易理解，因為每個概念都在系統中有實際角色，而不是只存在於單獨的小範例。

---

## Compact Slide Plan

Use this if you want to reduce the material into slides.

1. Title: Mini Jupyter Notebook Clone with C + ZeroMQ
2. Screenshot: browser UI
3. Main flow: Browser -> Server -> Broker -> Worker -> Runtime -> Browser
4. Click Run: what happens step by step
5. Socket API lifecycle: create, configure, bind/connect, send/receive
6. Broker pattern: ROUTER/DEALER shared queue
7. Multipart messages: RUN + JSON, RESULT + JSON
8. Worker: cumulative C execution
9. PUB/SUB status and `zmq_poll`
10. Extra concepts: PAIR, zero-copy, transport bridge
11. Live demo
12. Conclusion

---

## Presenter Checklist

Before presenting:

```bash
make
./build/broker
./build/kernel_worker
./build/server
```

Open:

```text
http://127.0.0.1:8080
```

Optional demo commands:

```bash
./build/pair_signal_demo
./build/zero_copy_demo
./build/transport_bridge_demo
```

Animation:

```text
animation/media/videos/architecture_animation/720p30/ZeroMQNotebookArchitecture.mp4
```
