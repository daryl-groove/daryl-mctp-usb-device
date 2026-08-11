# Bridge Backend Design — A1 / A2 共存架構

> **Status:** A1 已實作（嵌入 `ffs_daemon.c`）；bridge 介面抽離為下一個 refactor 步驟。
> A2 kernel module 尚未實作。
>
> 本文記錄「bridge 後端」的設計哲學與介面合約，確保 A1 不是過渡廢棄品，而是
> 永久保留的第一個 backend，與未來的 A2 平行共存。

---

## 1. 問題背景：為什麼要抽出 bridge 介面

`ffs_daemon.c` 的職責是：
- ep0 lifecycle（BIND / ENABLE / DISABLE events）
- poll loop（ep0 + ep_out + bridge fd）
- USB header encode / decode（`mctp_usb_frame.{h,c}`）

**只有 bridge 後端不同**於 A1 和 A2：

| 層 | A1 (PTY + mctp-serial) | A2 (kernel module chardev) |
|---|---|---|
| USB header strip/add | `mctp_usb_frame.c` ← **完全相同** | 完全相同 |
| ep0 / poll loop | `ffs_daemon.c` ← **完全相同** | 完全相同 |
| **bridge: 送出** | DSP0253 encode → write PTY master | write raw MCTP → `/dev/mctp-gadget` |
| **bridge: 接收** | read PTY master → DSP0253 decode state machine | read `/dev/mctp-gadget` → raw MCTP |
| **bridge: 初始化** | open /dev/ptmx + N_MCTP ldisc | open /dev/mctp-gadget |
| kernel 側介面 | mctpserial0（ARPHRD_MCTP via N_MCTP） | mctpgadget0（ARPHRD_MCTP via module） |

**結論：** 把 bridge 實作從 `ffs_daemon.c` 抽出，讓 `ffs_daemon.c` 只呼叫一個固定介面，
A1 / A2 分別實作這個介面，compile time 決定哪個被編進去。

---

## 2. 為什麼 A1 不是「臨時踏腳石，A2 完成後就廢棄」

A1 和 A2 的適用場景不同，應該永久共存：

| 面向 | A1 (PTY + mctp-serial) | A2 (kernel module) |
|------|------------------------|-------------------|
| 需要 kernel module | ✗（只需要 `CONFIG_MCTP_SERIAL=y`） | ✓（需要 `mctp-gadget.ko`） |
| 適用場景 | 現有 OpenBMC image 快速部署 | 長期生產環境 |
| 格式轉換開銷 | 有（DSP0253 encode/decode + CRC） | 無（raw MCTP packet 直通） |
| mctpusbd 邏輯複雜度 | 中（state machine） | 低（直接 read/write） |
| 上游潛力 | 低（PTY 是 workaround） | 高（kernel module 可提交上游） |

A1 在沒有自製 kernel module 的環境（標準 OpenBMC image、CI 環境）依然是合法的生產路徑。
A2 完成後，提供更好的效能和更乾淨的架構，但 A1 不應被移除。

---

## 3. A1 為什麼需要 DSP0253 framing（A2 為什麼不需要）

**根本原因：N_MCTP line discipline 是為實體 serial line 設計的。**

N_MCTP (`mctp-serial.c`, kernel 5.15+) 將 serial TTY 包裝成 MCTP net device。
Serial line 是 byte stream，沒有 message boundary，所以 N_MCTP 要求輸入符合
DSP0253 framing：

```
[0x7E][len_hi][len_lo][MCTP packet...][FCS_hi][FCS_lo][0x7E]
```

PTY 是「假的 serial line」。N_MCTP 不知道也不在乎對面是 UART 還是 PTY——
它只認 DSP0253。所以 mctpusbd A1 必須：
- 送出：把 raw MCTP packet 包成 DSP0253 frame，write to PTY master
- 接收：從 PTY master 讀 byte stream，用 state machine 解 DSP0253 frame

A2 的 kernel module 自己就是 MCTP net device，直接說 raw MCTP packet 語言。
mctpusbd 只要 `write(chardev_fd, mctp_pkt, len)` 就完成，不需要任何 framing。

```
A1 資料路徑（多了兩層格式轉換）：
  USB(DSP0283) → strip USB hdr → DSP0253 encode → PTY master →
    [kernel: PTY slave → N_MCTP deframe] → mctpserial0 → kernel MCTP routing

A2 資料路徑（最短路徑）：
  USB(DSP0283) → strip USB hdr → chardev write →
    [kernel: mctp-gadget.ko netif_rx()] → mctpgadget0 → kernel MCTP routing
```

---

## 4. Bridge 介面合約

### `ffs/ffs_bridge.h`（目標狀態，尚未抽出）

```c
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Forward declaration; defined in ffs_daemon.c */
typedef struct ffs_ctx ffs_ctx_t;

/*
 * Open and initialise the bridge backend.
 *
 * *bridge_fd : primary fd to include in the poll loop (PTY master or chardev).
 * *aux_fd    : secondary fd that must stay open for the bridge lifetime but is
 *              not polled (PTY slave, keeps N_MCTP attached). Set to -1 if unused.
 *
 * Returns 0 on success, -1 on error.
 */
int ffs_bridge_open(int *bridge_fd, int *aux_fd);

/*
 * Forward one inbound MCTP packet (already stripped of USB header) to the
 * kernel MCTP stack.  Called from handle_out() when bridge_fd is the target.
 */
void ffs_bridge_handle_out(ffs_ctx_t *ctx,
                           const uint8_t *mctp_pkt, size_t mctp_len);

/*
 * Read from bridge_fd and forward any complete MCTP packets to USB IN.
 * Called from the poll loop when bridge_fd is readable.
 */
void ffs_bridge_handle_in(ffs_ctx_t *ctx);

/*
 * Release resources.  Called on clean exit or error.
 */
void ffs_bridge_close(int bridge_fd, int aux_fd);
```

`ffs_daemon.c` 只呼叫這四個函數，完全不知道 A1/A2 的存在。

---

## 5. 檔案結構（目標狀態）

```
ffs/
  ffs_bridge.h          ← 共同介面宣告（A1/A2 都要實作）
  ffs_bridge_a1.c       ← A1 實作：PTY open + DSP0253 encode/decode + state machine
  ffs_bridge_a2.c       ← A2 實作：open /dev/mctp-gadget + raw MCTP read/write
  ffs_pty.h             ← 只被 ffs_bridge_a1.c 使用
  ffs_pty.c             ← 只被 ffs_bridge_a1.c 使用
  ffs_daemon.h          ← FFS_MODE_BRIDGE（不分 A1/A2）
  ffs_daemon.c          ← 呼叫 ffs_bridge.h 介面；零 #ifdef

mctp/
  mctp_serial_frame.h   ← 只被 ffs_bridge_a1.c 使用
  mctp_serial_frame.c   ← 只被 ffs_bridge_a1.c 使用
  mctp_usb_frame.h/c    ← A1/A2 共用
  mctp_packet.h/c       ← ffsd standalone mode 用
  mctp_control.h/c      ← ffsd standalone mode 用
  mctp_endpoint.h/c     ← ffsd standalone mode 用

app/
  ffsd.c                ← FFS_MODE_MCTP（standalone MCTP responder，不走 bridge）
  mctpusbd.c            ← FFS_MODE_BRIDGE（呼叫 ffs_bridge_open，bridge backend 由編譯決定）
```

---

## 6. Meson 編譯切換

### `meson_options.txt`（新增）

```ini
option('bridge',
  type        : 'combo',
  choices     : ['a1', 'a2'],
  value       : 'a1',
  description : 'MCTP bridge backend — a1: PTY+mctp-serial (no kernel module needed); \
                 a2: mctp-gadget.ko chardev (cleaner, lower overhead)')
```

### `meson.build` 選擇邏輯（目標狀態）

```python
bridge_opt = get_option('bridge')

if bridge_opt == 'a1'
  bridge_sources     = files('ffs/ffs_bridge_a1.c', 'ffs/ffs_pty.c')
  mctp_bridge_sources = files('mctp/mctp_serial_frame.c')
  add_project_arguments('-DBRIDGE_A1=1', language : 'c')
elif bridge_opt == 'a2'
  bridge_sources     = files('ffs/ffs_bridge_a2.c')
  mctp_bridge_sources = []
  add_project_arguments('-DBRIDGE_A2=1', language : 'c')
endif
```

### Build 指令

```bash
# A1（預設，不需要 kernel module）
meson setup build-a1 -Dbridge=a1
ninja -C build-a1

# A2（需要 mctp-gadget.ko）
meson setup build-a2 -Dbridge=a2
ninja -C build-a2
```

---

## 7. 當前狀態 vs 目標狀態

### 當前狀態（A1 完成，尚未抽介面）

```
ffs_daemon.c
  └─ FFS_MODE_PTY_BRIDGE 的邏輯直接嵌在：
       handle_out()        ← DSP0253 encode + write PTY master
       handle_pty_master() ← state machine + DSP0253 decode + write ep-IN
       ffs_serve()         ← pty_master_fd 作為參數傳入
```

A1 的 bridge 邏輯目前「散落」在 `ffs_daemon.c` 中。功能正確，但與
ep0 lifecycle 代碼耦合，不利於之後加入 A2。

### 目標狀態（bridge 介面抽出後）

```
ffs_daemon.c
  └─ handle_out()  →  mctp_usb_decode() + ffs_bridge_handle_out()
  └─ poll loop     →  ffs_bridge_handle_in()
  └─ ffs_serve()  →  ffs_bridge_open() / ffs_bridge_close()
     ← 完全不知道 A1/A2，零 #ifdef
```

---

## 8. Refactor 步驟（A1 介面抽離）

這個 refactor **不改變行為**，只搬移代碼：

1. 新增 `ffs/ffs_bridge.h` — 宣告 4 個函數
2. 建立 `ffs/ffs_bridge_a1.c` — 從 `ffs_daemon.c` 搬入：
   - `pty_rx_state_t` 和 state machine（`pty_rx_reset`, `pty_rx_feed`, `pty_rx_dispatch`）
   - `handle_pty_master()` → 改名為 `ffs_bridge_handle_in()`
   - PTY_BRIDGE case in `handle_out()` → `ffs_bridge_handle_out()`
   - PTY open 邏輯 → `ffs_bridge_open()`
3. `ffs_daemon.c` 改呼叫 `ffs_bridge.h` 介面，刪掉被搬走的代碼
4. `meson.build` 加入 option + 條件式 source 選擇
5. `FFS_MODE_PTY_BRIDGE` → `FFS_MODE_BRIDGE`（不分 A1/A2）

---

## 9. A2 kernel module 需要實作的部分

A2 的 `ffs_bridge_a2.c`（userspace）很簡單（~50 行），主要工作在 kernel module。

### `ffs_bridge_a2.c`（userspace side）

```c
int  ffs_bridge_open(int *bridge_fd, int *aux_fd)
  → open("/dev/mctp-gadget", O_RDWR)
  → *bridge_fd = chardev_fd; *aux_fd = -1

void ffs_bridge_handle_out(ffs_ctx_t *ctx, const uint8_t *pkt, size_t len)
  → write(chardev_fd, pkt, len)  /* raw MCTP packet, no framing */

void ffs_bridge_handle_in(ffs_ctx_t *ctx)
  → n = read(chardev_fd, buf, sizeof(buf))
  → mctp_usb_encode(buf, n, usb_frame, ...)
  → write_in_frame(ctx, usb_frame, usb_len)

void ffs_bridge_close(int bridge_fd, int aux_fd)
  → close(bridge_fd)
```

### `mctp_gadget.ko`（kernel module，~300 行）

```
module_init
  ├─ misc_register()         → /dev/mctp-gadget
  └─ alloc_netdev() + register_netdev()  → mctpgadget0 (ARPHRD_MCTP)

net_device_ops
  ├─ ndo_open / ndo_stop     → netif_carrier_on/off
  └─ ndo_start_xmit(skb)     → push skb to kfifo → wake read()

file_operations
  ├─ open / release          → set/clear "connected" flag
  ├─ write(pkt)              → skb_alloc + copy + netif_rx()  [RX inject]
  ├─ read()                  → pop kfifo → copy_to_user()     [TX drain]
  └─ poll()                  → wait_event (kfifo not empty)
```

---

## 10. Runtime 依賴關係

```
mctpusbd-a1  →  需要：CONFIG_MCTP_SERIAL=y（標準 OpenBMC kernel 通常有）
mctpusbd-a2  →  需要：mctp-gadget.ko 已載入（/dev/mctp-gadget 存在）

兩者都需要：
  CONFIG_MCTP=y              kernel MCTP routing layer
  AF_MCTP socket support     pldmd 才能連上
```

A2 的 userspace 和 kernel module 在 **compile time 互相獨立**；
runtime 依賴透過 `open("/dev/mctp-gadget")` 的成功/失敗體現。
Module 未載入時 mctpusbd 啟動即失敗並印出清楚的錯誤訊息。
