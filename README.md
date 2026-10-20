<img width="1706" height="1279" alt="dc8338977367e77b6b3a2dc8e7ed0fc0" src="https://github.com/user-attachments/assets/a696f508-f2bb-4e4f-9c62-8f1709ea99bd" />
<img width="1706" height="1279" alt="ff65e3185f4e010b171385342404c463" src="https://github.com/user-attachments/assets/04ca0f6e-0014-4870-8350-db828508bc43" />
# RK3568 Linux 字符设备驱动（LED + 按键）与 Qt MQTT 远程控制

基于 RK3568 开发板的完整嵌入式 Linux 项目，覆盖**内核驱动 → 用户态应用 → 网络通信**全链路。

底层用 platform 驱动框架实现 LED + 按键字符设备，通过设备树获取 GPIO 资源；上层用 Qt 开发 GUI 应用，通过 MQTT 实现远程控制与状态上报。

**技术栈**：C（Linux 内核模块）· C++ / Qt · MQTT · 设备树 · 交叉编译

---

## 一、硬件与环境

| 项目 | 说明 |
|---|---|
| 开发板 | RK3568 |
| 外设 | LED（GPIO 输出）、按键（GPIO 下降沿中断） |
| 内核态 | Linux 字符设备驱动（platform 框架 + gpiod 子系统） |
| 用户态 | Qt 5.x GUI + QtMqtt |
| MQTT Broker | 公共 broker（`test.mosquitto.org:1883`） |
| 构建 | 交叉编译工具链 + 内核模块构建系统 |

---

## 二、系统架构

```
   ┌──────────────────────────────────────────────────────┐
   │                    用户空间                            │
   │   ┌────────────────────────────────────────────┐      │
   │   │           Qt GUI 应用（C++）                │      │
   │   │  ┌──────────────┐    ┌──────────────────┐  │      │
   │   │  │ KeyReaderThr │    │   MainWindow     │  │      │
   │   │  │  后台线程     │───►│  信号槽 / 界面    │  │      │
   │   │  │ 阻塞 read()  │信号 │  MQTT 客户端      │  │      │
   │   │  └──────┬───────┘    └────────┬─────────┘  │      │
   │   └─────────┼─────────────────────┼────────────┘      │
   │             │ open/read/write     │ MQTT              │
   └─────────────┼─────────────────────┼──────────────────┘
                 │                     ▼
        ┌────────┴────────┐     ┌──────────────┐
        │  /dev/myled     │     │ MQTT Broker  │◄──── 其他客户端
        └────────┬────────┘     └──────────────┘
                 │ 系统调用
   ┌─────────────┴──────────────────────────────────────┐
   │                   内核空间                           │
   │  字符设备驱动（platform driver）                      │
   │  ┌────────┬─────────┬─────────┬──────────┬────────┐ │
   │  │ open   │ write   │ read    │ poll     │ remove │ │
   │  │引用计数 │ LED 控制 │ 阻塞读  │ 多路复用  │ 安全卸载│ │
   │  └────────┴─────────┴─────────┴──────────┴────────┘ │
   │         ▲ 中断 + 延时工作队列（20ms 防抖）              │
   └─────────┼──────────────────────────────────────────┘
             │
        ┌────┴─────┐
        │ GPIO 按键 │
        └──────────┘
```

**数据流**

- **按键** → GPIO 下降沿中断 → 延时工作队列防抖 → 置可读标志 + 唤醒等待队列 → 用户态 `read` 返回 → 后台线程发信号 → 主线程刷新界面 + MQTT 上报
- **远程控制** → 其他 MQTT 客户端发布到 `cmd` 主题 → 应用收到 → `write(fd, "1"/"0")` → 驱动操作 GPIO

---

## 三、驱动设计

### 1. platform 驱动框架与资源管理

通过设备树的 `compatible` 属性匹配硬件：

```dts
myled {
    compatible = "topeet,myled";
    led-gpios  = <&gpioX XX GPIO_ACTIVE_HIGH>;
    key-gpios  = <&gpioX XX GPIO_ACTIVE_LOW>;
};
```

- **probe**：申请 GPIO、中断号、设备号等资源并完成初始化，每一步失败都逐级回滚已申请的资源
- **remove**：按与 probe **相反的顺序**释放资源，避免泄漏

### 2. 字符设备注册

标准四步流程：

```
alloc_chrdev_region  →  cdev_init / cdev_add  →  class_create  →  device_create
   （分配设备号）           （注册字符设备）          （创建设备类）     （生成设备节点）
```

最终生成 `/dev/myled`，应用层通过标准系统调用即可操作硬件。

### 3. LED 写入控制

实现 `write` 接口：从用户空间拷贝 1 字节，`'1'` 点亮、`'0'` 熄灭。

- 使用 **gpiod 子系统**操作 GPIO（`gpiod_set_value_cansleep`）
- 写入过程加 **互斥锁**，防止多进程并发写入导致 GPIO 状态混乱

### 4. 按键防抖：中断 + 延时工作队列

```
GPIO 下降沿中断
    ↓
中断处理函数：只调用 mod_delayed_work(20ms)     ← 不在中断上下文做耗时操作
    ↓
按键抖动会不断重置这个 20ms
    ↓
按键稳定后，延时工作队列回调执行：
    读取 GPIO 电平 → 判断按下/松开 → 切换 LED（可选）
```

**要点**：
- 中断处理函数**必须短**，所以只做"重新排期"
- 用 `mod_delayed_work` 而不是 `schedule_delayed_work` —— 后者在已有排期时会失败，前者会**重置**计时，这正是防抖需要的语义
- 回调中访问 GPIO 使用 `_cansleep` 版本（工作队列上下文允许睡眠）

### 5. 阻塞读取与 poll 机制

`read` 用**等待队列 + 原子变量**实现阻塞：

```c
while (1) {
    if (已移除)              return -ENODEV;
    if (atomic_xchg(&flag, 0))   break;        // 取走事件（原子交换，保证只被一个进程取到）
    if (O_NONBLOCK)          return -EAGAIN;   // 非阻塞模式直接返回
    wait_event_interruptible(wq, flag || removed);   // 阻塞休眠
}
```

**设计要点**：

| 机制 | 作用 |
|---|---|
| `atomic_xchg` | 原子地"读取并清零"标志位，保证**每个按键事件只会被一个读进程取走** |
| "先查标志再睡眠"的循环 | 避免**丢唤醒**（lost wakeup）——如果先睡眠再检查，可能错过中断 |
| `O_NONBLOCK` 分支 | 支持非阻塞读取，返回 `-EAGAIN` |
| `removed` 参与唤醒条件 | 模块卸载时能唤醒所有阻塞的读进程 |

同时实现 `poll` 接口，应用层可用 `epoll` / `poll` 监听按键事件：

- 有事件 → `POLLIN | POLLRDNORM`
- 模块已卸载 → `POLLERR | POLLHUP`

### 6. 安全卸载（重点）

模块卸载时若仍有进程持有设备文件，直接释放资源会导致**访问已失效的 GPIO**。这里做了三层保护：

**① 文件引用计数**：`open` 时 `atomic_inc`，`release` 时 `atomic_dec_and_test` 并唤醒卸载等待队列

**② 卸载标志位 + 双检模式**：解决"加引用期间模块被卸载"的时序漏洞

```c
/* led_open：先加引用，再复查标志 */
if (atomic_read(&priv->removed)) return -ENODEV;
atomic_inc(&priv->open_count);
if (atomic_read(&priv->removed)) {          /* 复查：期间可能已被移除 */
    if (atomic_dec_and_test(&priv->open_count)) wake_up(&remove_wq);
    return -ENODEV;
}
```

```c
/* led_write：copy_from_user 之后拿锁再复查一次 */
ret = copy_from_user(kbuf, buf, 1);
mutex_lock(&priv->lock);
if (atomic_read(&priv->removed)) {          /* 拿到锁后资源仍然有效 */
    mutex_unlock(&priv->lock);
    return -ENODEV;
}
```

**③ 卸载时阻塞等待**：`remove` 中唤醒所有读进程后，等待引用计数归零才释放资源

```
remove 流程（顺序很重要）：
  1. 置 removed 标志 + 唤醒等待队列（让阻塞的调用返回）
  2. wait_event(引用计数归零)          ← 等应用层都关闭文件
  3. free_irq + cancel_delayed_work_sync  ← 先切断中断和延时工作，保证不再有人碰 GPIO
  4. 关灯 + 注销设备节点 / 设备类 / 设备号
  5. 最后 gpiod_put 释放 GPIO
```

**第 3 步在第 4、5 步之前是关键** —— 先切断新的操作来源，再释放资源。

---

## 四、上层应用设计

### 1. 多线程与跨线程界面更新

自定义线程类继承 `QThread`，后台线程**阻塞调用 `read`** 读取按键事件，避免阻塞 GUI 主线程。

**跨线程更新界面的正确姿势**：

> Qt 规定**只有主线程能操作 GUI 控件**。子线程里不直接改控件，而是 `emit` 一个信号，槽函数在主线程的事件循环中执行（默认 `QueuedConnection`），从而安全刷新界面。

```
后台线程：read() 返回 → emit keyPressed()
                              ↓（跨线程排队）
主线程：  槽函数执行 → 更新计数标签 + 发布 MQTT 状态
```

### 2. 线程安全退出

`stop()` 的顺序很关键：

```cpp
m_running.store(false);      // ① 先清运行标志
const int fd = m_fd;
::close(fd);                 // ② 再 close 文件描述符
m_fd = -1;
```

**原理**：`close` 会让正在阻塞的 `read()` 立即返回错误，`run()` 循环随即退出。如果只清标志位，阻塞中的 `read` 不会被唤醒，线程就退不出来。

### 3. LED 控制封装

把系统调用 `open` / `write` 封装成 `setLed()`：控制 LED 的同时同步界面状态，失败时在界面上显示错误提示。

**细节**：`write` 循环处理 `EINTR`（被信号打断不是错误，重试）；错误码先保存下来再使用，避免后续调用覆盖 `errno`。

### 4. MQTT 通信与可靠性

**主题设计**

| 主题 | 方向 | 内容 |
|---|---|---|
| `topeet/led/cmd` | 订阅 | `"1"` / `"0"` → 控制 LED 亮灭 |
| `topeet/led/status` | 发布 | `online` / `key_pressed` / `offline`（遗嘱） |

**可靠性设计**

| 机制 | 实现 |
|---|---|
| **断线重连** | 断线后启动 5 秒单次定时器，回调中尝试重连；**启动前检查定时器状态**，已激活则不重复 `start`，避免无效重连堆积 |
| **遗嘱消息** | 设置 Will 为 `offline` 并开启 **retain** —— 设备异常掉线时由 broker 自动发布，远端能立刻感知 |
| **上线状态** | 连接成功后发布 `online`（同样 retain），新订阅者可立即获知设备状态 |
| **心跳** | KeepAlive 30 秒 |

**为什么遗嘱要开 retain**：新订阅者订阅 `status` 主题时，如果没有 retain，必须等到下次状态变更才知道设备是否在线；开启后 broker 会立即推送最后一次状态。

---

## 五、代码结构

```
.
├── driver/                 Linux 内核模块
│   └── led.c                   platform 驱动：LED + 按键字符设备
└── app/                    Qt 上位机
    ├── mainwindow.*            界面、MQTT 客户端、LED 控制
    └── keyreaderthread.*       后台线程阻塞读取按键

```

---

## 六、编译与运行

1. **设备树**：添加 `myled` 节点，配置 `compatible` 与 GPIO
2. **驱动**：交叉编译内核模块 → `insmod led.ko` → 确认 `/dev/myled` 生成
3. **应用**：交叉编译 Qt 程序 → 在开发板运行 → 连接 broker
4. **验证**：
   - 按开发板按键 → 界面计数 +1，且 MQTT 收到 `key_pressed`
   - 用其他 MQTT 客户端向 `topeet/led/cmd` 发布 `"1"` → LED 点亮
   - `rmmod led` → 观察是否等待文件关闭后才释放

---

## 七、已知限制

- **卸载可能阻塞**：`remove` 中使用的是不可中断的 `wait_event`，若应用层长期不关闭文件，`rmmod` 会一直等待（且无法被信号打断）
- **未使用 `devm_*` 系列接口**：GPIO、中断、设备号均为手动管理，错误路径需逐级回滚；改用 `devm_gpiod_get` / `devm_request_irq` 可减少遗漏风险
- **中断触发类型硬编码**：`request_irq` 中写死了 `IRQF_TRIGGER_FALLING`，规范做法是由设备树描述触发方式，驱动不写死
- **使用公共 MQTT broker**：`test.mosquitto.org` 仅适合演示，不稳定也不安全；实际产品应自建 broker 并加入认证
- **MQTT QoS 0**：状态消息为至多一次，弱网下可能丢失；关键指令应提升 QoS 等级
- **控制 LED 时每次重新 `open`**：低频操作下可接受，高频场景应保持 fd 常开

---

## 八、说明

本项目独立完成了**设备树修改、驱动编写、交叉编译、模块加载调试、Qt 应用开发**的完整流程，对 Linux 驱动开发流程以及「应用层与驱动的交互方式」有了完整的理解。

关键设计集中在**内核态的并发与生命周期管理**：中断上下文的职责边界、阻塞读取的丢唤醒规避、以及模块卸载时的竞态处理。
