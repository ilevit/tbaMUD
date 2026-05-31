# Proposal: Transitioning tbaMUD Core Event Loop to libuv

## 1. Objective
Rewrite the core event loop in `src/comm.c` to use `libuv`. This will replace the aging `select()`-based synchronous loop with a modern, asynchronous, event-driven architecture that scales better on modern operating systems (using epoll on Linux, kqueue on BSD/macOS, and IOCP on Windows).

## 2. Dependencies
- **libuv**: Integrate `libuv` (>= 1.0.0) into the build system.
- **CMake**: Update `CMakeLists.txt` to find and link `libuv`.
- **conf.h**: Add `HAVE_LIBUV` detection.

## 3. Structural Changes

### 3.1. Descriptor Data (`src/structs.h`)
Add `libuv` handles to the `descriptor_data` structure to manage connection state and I/O asynchronously.
```c
struct descriptor_data {
  socket_t descriptor;      /* Raw socket (keep for legacy/info) */
  uv_tcp_t handle;          /* libuv TCP handle */
  uv_write_t write_req;     /* libuv write request */
  // ... existing fields ...
};
```

### 3.2. Global Handles (`src/comm.c`)
Introduce global libuv handles to manage the loop and periodic tasks.
```c
uv_loop_t *loop;
uv_tcp_t mother_handle;
uv_timer_t heartbeat_timer;
```

## 4. Implementation Steps

### 4.1. Initialization (`init_game`)
- Initialize the libuv default loop: `loop = uv_default_loop();`
- Initialize the heartbeat timer: `uv_timer_init(loop, &heartbeat_timer);`
- Start the timer with a 100ms interval (matching `OPT_USEC`).

### 4.2. Networking Refactor (`init_socket` & `new_descriptor`)
- Replace raw `bind()`/`listen()` calls with `uv_tcp_bind()` and `uv_listen()`.
- Implement a `on_new_connection` callback:
    - Calls `uv_accept()`.
    - Allocates and initializes `descriptor_data`.
    - Starts reading via `uv_read_start()`.

### 4.3. I/O Callbacks
- **Read Callback (`on_read`)**: Replaces the input-polling logic. It will feed received data into the existing `process_input()` logic to populate the command queue.
- **Write Callback (`on_write`)**: Handles completion of asynchronous sends.
- **Buffer Allocation (`alloc_buffer`)**: Provides memory for libuv's read operations.

### 4.4. Heartbeat and Command Processing
The `heartbeat_cb` (timer callback) will become the primary driver:
1. Call `heartbeat(++pulse)` to handle world updates.
2. Iterate through `descriptor_list` to:
    - Process one command from the `input` queue (maintaining game balance).
    - Check for connection timeouts or `CON_CLOSE` states.
    - Generate prompts if needed and trigger `uv_write`.

### 4.5. Signal Handling
Replace `signal_setup()` with `uv_signal_t` handles for `SIGINT`, `SIGHUP`, and `SIGUSR` to ensure clean shutdowns and signal processing within the event loop.

## 5. Benefits
- **Performance:** Reduced overhead when handling hundreds of idle connections.
- **Efficiency:** Uses native OS event notification systems (epoll/kqueue/IOCP).
- **Portability:** `libuv` provides a consistent API across all supported tbaMUD platforms.
- **Modernization:** Lays the groundwork for future features like non-blocking DNS resolution or asynchronous file I/O.

## 6. Main Loop Replacement
The `game_loop` function will be drastically simplified:
```c
void game_loop(socket_t local_mother_desc) {
  /* ... setup already done in init_game ... */
  log("Entering libuv event loop.");
  uv_run(loop, UV_RUN_DEFAULT);
  log("Exiting event loop.");
}
```
