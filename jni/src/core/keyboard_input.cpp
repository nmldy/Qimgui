#include "keyboard_input.h"

#include <atomic>
#include <cerrno>        // errno, EAGAIN, EINTR
#include <cstdint>
#include <cstdio>        // std::snprintf
#include <cstring>       // std::strncmp
#include <dirent.h>      // opendir / readdir / closedir
#include <fcntl.h>       // open / O_RDONLY / O_NONBLOCK
#include <linux/input.h> // input_event / EV_KEY / KEY_* / EVIOCGBIT
#include <sys/ioctl.h>   // ioctl
#include <thread>
#include <unistd.h>      // read / close / usleep
#include <vector>

// AImGui 加固版音量键读取。
//
// 设计原则（按优先级）：
//
//   1. 只接受"纯音量键设备"——有音量键，且没有任何字母键。
//      音量键在 Android 上由 gpio-keys 节点上报，该节点天然不含
//      KEY_A..KEY_Z。这样一来，即便有人把下面的过滤逻辑改回
//      "捕获所有按键"，那个 fd 上也不存在可读的文本输入，改动
//      没有收益。
//   2. 只读打开（O_RDONLY）。不写、不注入、不创建 uinput 设备。
//   3. 不保存按键数据。没有映射表、没有队列、没有 mutex，唯一被
//      写出的状态是一个原子计数器（音量按下次数）。
//   4. Flush() 为空操作——本模块从不向 ImGui 注入任何输入。
//   5. 最多同时打开 4 个设备，避免被用于刷 fd。
//
// 仍然需要 root 才能打开 /dev/input。本文件无法改变这一前提，但可以
// 保证：这一段代码原样编译时，除了"数了一下音量键按了几次"，不会保留
// 任何其他信息。
namespace aimgui::kbd_input {

namespace {

constexpr int kMaxDevices = 4;

std::atomic<bool>        g_running{false};
std::vector<int>         g_fds;
std::vector<std::thread> g_threads;
std::atomic<int>         g_volume_presses{0};

// 判据：有音量键，且不含任何字母键。
// gpio-keys 满足前者、不满足后者被拒的条件，因此会被接受；
// 真实键盘（含字母键）会被拒绝，即使它也有音量键。
bool IsPureVolumeKeyDevice(int fd) {
    uint8_t bits[(KEY_MAX / 8) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;

    auto has = [&](int k) -> bool {
        return (bits[k / 8] & (1u << (k % 8))) != 0;
    };

    // 有任何字母键 → 不是纯音量键设备 → 拒绝。
    for (int k = KEY_A; k <= KEY_Z; ++k) {
        if (has(k)) return false;
    }

    // 有数字键行也一并拒绝（软键盘/复合 HID 常见）。
    for (int k = KEY_0; k <= KEY_9; ++k) {
        if (has(k)) return false;
    }

    return has(KEY_VOLUMEUP) || has(KEY_VOLUMEDOWN);
}

void ReaderLoop(int fd) {
    input_event ev[16];
    while (g_running.load(std::memory_order_relaxed)) {
        ssize_t n = ::read(fd, ev, sizeof(ev));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                usleep(5'000);
                continue;
            }
            break;
        }
        const size_t count = static_cast<size_t>(n) / sizeof(input_event);
        for (size_t i = 0; i < count; ++i) {
            // 只认音量键的按下边沿。其它任何事件类型、任何编码、
            // 任何 value 都在此处被丢弃：不入队、不保存、不转发。
            if (ev[i].type != EV_KEY)  continue;
            if (ev[i].value != 1)      continue;
            if (ev[i].code == KEY_VOLUMEUP || ev[i].code == KEY_VOLUMEDOWN) {
                g_volume_presses.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace

void Init() {
    if (g_running.exchange(true)) return;

    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        if (static_cast<int>(g_fds.size()) >= kMaxDevices) break;
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;

        char path[64];
        std::snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);

        // O_RDONLY：只读，不写。O_NONBLOCK：不阻塞枚举。
        int fd = ::open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (!IsPureVolumeKeyDevice(fd)) { ::close(fd); continue; }

        g_fds.push_back(fd);
        g_threads.emplace_back(ReaderLoop, fd);
    }
    closedir(dir);
}

void Shutdown() {
    g_running.store(false);

    // Fix: join the readers BEFORE closing their fds. The original order
    // (close-then-detach) raced with a thread that could still be inside
    // read(fd, ...) — the kernel may recycle the fd number to an unrelated
    // open() between our close() and the thread's next syscall. Because
    // every fd was opened O_NONBLOCK, a reader that hits EAGAIN sleeps at
    // most 5 ms before re-checking g_running, so joining here is bounded.
    for (auto& t : g_threads) if (t.joinable()) t.join();
    g_threads.clear();

    for (int fd : g_fds) ::close(fd);
    g_fds.clear();
    g_volume_presses.store(0, std::memory_order_relaxed);
}

void Flush() {
    // 空操作：本模块不向 ImGui 注入任何按键或字符。
}

int ConsumeVolumePresses() {
    return g_volume_presses.exchange(0, std::memory_order_relaxed);
}

} // namespace aimgui::kbd_input