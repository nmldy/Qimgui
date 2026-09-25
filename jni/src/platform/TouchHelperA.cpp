#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <cerrno>         // errno / EAGAIN / EINTR
#include <cstdio>         // std::snprintf
#include <cstring>        // memset / strstr
#include <linux/input.h>
#include <sys/ioctl.h>    // ioctl
#include <pthread.h>      // Fix: pthread_create / pthread_join / pthread_t
#include <vector>
#include <thread>
#include "spinlock.h"

#include "imgui.h"
#include "TouchHelperA.h"

#define maxF 10

// AImGui 加固版触摸读取。
//
// 与原始实现的关键差异：
//   - 绝不 EVIOCGRAB：系统继续收到原始触摸，下层 App 照常工作。
//   - 绝不打开 /dev/uinput：编译产物中不再包含 uinput 代码路径。
//   - 绝不调用 Upload()：触摸事件不被重注入任何地方。
//   - Down / Move / Up / SetCallBack 保留声明与定义，实现为空操作。
//   - Init 的 p_readOnly 参数被忽略，强制按只读执行。
//   - 只 O_RDONLY | O_NONBLOCK 打开设备。
//   - latest（ABS_MT_SLOT 值）clamp 到 [0, maxF-1]，避免数组越界。
//   - 多指 MouseDown 判定改为“任意手指按下”，避免误清。
//
// 唯一保留的能力：读原始触摸坐标 → 喂 ImGui 的 io.MousePos /
// io.MouseDown[0]，用于驱动悬浮窗 UI。
//
// 打开 /dev/input 需要 root。本文件无法改变这一前提，但可以保证：
// 编译产物中不含注入路径，唯一写出的状态是 ImGui 的当前帧鼠标状态。
namespace Touch {

    static My_Vector2          touch_scale;
    static My_Vector2          screenSize;
    static std::vector<Device> devices;
    // Fix: keep the pthread handles so Close() can join before tearing the
    // devices vector down. Previously the threads were detached and could
    // still hold `Device& device = devices[i]` while Close() ran
    // devices.clear() — a use-after-free.
    static std::vector<pthread_t> readerThreads;
    static int                 orientation = 0;
    static bool                initialized = false;
    static bool                otherTouch  = false;
    static spinlock            lock;

    // 判断设备是不是多点触控屏：有 SLOT + POSITION_X + POSITION_Y 即可。
    static bool checkDeviceIsTouch(int fd) {
        uint8_t* bits = nullptr;
        ssize_t  bits_size = 0;
        int      res = 0;
        bool     hasSlot = false, hasX = false, hasY = false;
        struct input_absinfo abs{};

        while (true) {
            res = ioctl(fd, EVIOCGBIT(EV_ABS, bits_size), bits);
            if (res < bits_size) break;
            bits_size = res + 16;
            bits = (uint8_t*)realloc(bits, bits_size * 2);
        }
        for (int j = 0; j < res; ++j) {
            for (int k = 0; k < 8; ++k) {
                if ((bits[j] & (1 << k)) && ioctl(fd, EVIOCGABS(j * 8 + k), &abs) == 0) {
                    if      (j * 8 + k == ABS_MT_SLOT)        hasSlot = true;
                    else if (j * 8 + k == ABS_MT_POSITION_X)  hasX    = true;
                    else if (j * 8 + k == ABS_MT_POSITION_Y)  hasY    = true;
                }
            }
        }
        free(bits);
        return hasSlot && hasX && hasY;
    }

    // 只观察触摸，喂 ImGui。不注入、不回调、不保存历史。
    static void* TypeA(void* arg) {
        int      i      = (int)(long)arg;
        Device&  device = devices[i];

        int          latest = 0;
        input_event  inputEvent[64]{0};

        while (initialized) {
            ssize_t readSize = read(device.fd, inputEvent, sizeof(inputEvent));
            if (readSize < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    usleep(5'000);
                    continue;
                }
                break;
            }
            if (readSize == 0 || (readSize % (ssize_t)sizeof(input_event)) != 0) {
                usleep(1'000);
                continue;
            }
            size_t count = (size_t)readSize / sizeof(input_event);

            lock.lock();
            for (size_t j = 0; j < count; ++j) {
                input_event& ie = inputEvent[j];

                if (ie.type == EV_ABS) {
                    if (ie.code == ABS_MT_SLOT) {
                        // clamp 到合法范围，避免越界访问 Finger[]。
                        int slot = ie.value;
                        if (slot < 0)        slot = 0;
                        if (slot >= maxF)    slot = maxF - 1;
                        latest = slot;
                        continue;
                    }
                    if (ie.code == ABS_MT_TRACKING_ID) {
                        if (ie.value == -1) {
                            device.Finger[latest].isDown = false;
                        } else {
                            device.Finger[latest].id     = (i * 2 + 1) * maxF + latest;
                            device.Finger[latest].isDown = true;
                        }
                        continue;
                    }
                    if (ie.code == ABS_MT_POSITION_X) {
                        device.Finger[latest].id     = (i * 2 + 1) * maxF + latest;
                        device.Finger[latest].pos.x  = (float)ie.value * device.S2TX;
                        continue;
                    }
                    if (ie.code == ABS_MT_POSITION_Y) {
                        device.Finger[latest].id     = (i * 2 + 1) * maxF + latest;
                        device.Finger[latest].pos.y  = (float)ie.value * device.S2TY;
                        continue;
                    }
                }

                if (ie.code == SYN_REPORT) {
                    if (ImGui::GetCurrentContext() != nullptr) {
                        ImGuiIO& io = ImGui::GetIO();

                        // 多指场景：只要任意一根手指还按着，就保持按下。
                        const touchObj* active = nullptr;
                        for (int f = 0; f < maxF; ++f) {
                            if (device.Finger[f].isDown) {
                                active = &device.Finger[f];
                                break;
                            }
                        }

                        if (active != nullptr) {
                            auto pos        = Touch2Screen(active->pos);
                            io.MousePos     = ImVec2(pos.x, pos.y);
                            io.MouseDown[0] = true;
                        } else {
                            io.MouseDown[0] = false;
                        }
                    }
                    // 不调用 Upload()，不触发 callback。只喂 ImGui。
                    continue;
                }
            }
            lock.unlock();
        }
        return nullptr;
    }

    bool Init(const My_Vector2& s, bool /*p_readOnly 忽略，强制只读*/) {
        Close();
        devices.clear();
        readerThreads.clear();

        My_Vector2 size = s;
        if (size.x > size.y) screenSize = size;
        else                 screenSize = { size.y, size.x };

        DIR* dir = opendir("/dev/input/");
        if (!dir) return false;

        char temp[128];
        // 扫描固定范围，避免依赖 readdir 计数的不确定性。
        for (int i = 0; i < 32; ++i) {
            std::snprintf(temp, sizeof(temp), "/dev/input/event%d", i);

            // O_RDONLY：只读打开。O_NONBLOCK：不阻塞枚举。
            int fd = open(temp, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;

            if (checkDeviceIsTouch(fd)) {
                Device device{};
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &device.absX) == 0 &&
                    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &device.absY) == 0) {
                    device.fd = fd;
                    // 不 EVIOCGRAB。系统继续收到触摸。
                    devices.push_back(device);
                } else {
                    close(fd);
                }
            } else {
                close(fd);
            }
        }
        closedir(dir);

        if (devices.empty()) return false;

        int screenX = devices[0].absX.maximum;
        int screenY = devices[0].absY.maximum;

        initialized = true;

        // Fix: keep the pthread_t so Close() can join. Also don't detach —
        // a detached thread could still be touching devices[i] when
        // devices.clear() runs.
        for (size_t i = 0; i < devices.size(); ++i) {
            devices[i].S2TX = (float)screenX / (float)devices[i].absX.maximum;
            devices[i].S2TY = (float)screenY / (float)devices[i].absY.maximum;
            pthread_t t;
            if (pthread_create(&t, nullptr, TypeA, (void*)(long)i) == 0) {
                readerThreads.push_back(t);
            }
        }

        if (size.x > size.y) std::swap(size.x, size.y);
        if (otherTouch)      std::swap(size.x, size.y);

        touch_scale.x = (float)screenX / size.x;
        touch_scale.y = (float)screenY / size.y;

        return true;
    }

    void Close() {
        if (!initialized) return;

        // Fix: order matters.
        //   1) signal readers to exit
        //   2) join — readers poll `initialized` every <=5 ms
        //   3) only then close fds and drop the devices vector
        // Previously we closed+cleared first and detached, so a reader
        // could hold a stale reference into `devices` (use-after-free).
        initialized = false;
        for (pthread_t t : readerThreads) pthread_join(t, nullptr);
        readerThreads.clear();

        for (auto& device : devices) {
            if (device.fd > 0) close(device.fd);
            device.fd = 0;
        }
        devices.clear();
    }

    // ── 兼容接口：全部为空操作 ────────────────────────────────────────
    // 这些函数在旧版里用于通过 /dev/uinput 重注入触摸事件。加固后
    // 保留签名以兼容调用点，但不再产生任何副作用。
    void Down(float /*x*/, float /*y*/)                        {}
    void Move(float /*x*/, float /*y*/)                        {}
    void Up()                                                   {}
    void Move(touchObj* /*touch*/, float /*x*/, float /*y*/)   {}
    void Upload()                                               {}
    void SetCallBack(const std::function<void(std::vector<Device>*)>& /*cb*/) {}

    // ── 只读查询接口：语义不变 ────────────────────────────────────────
    My_Vector2 Touch2Screen(const My_Vector2& coord) {
        float x = coord.x, y = coord.y;
        float xt = x / touch_scale.x;
        float yt = y / touch_scale.y;

        if (otherTouch) {
            switch (orientation) {
                case 1: x = xt; y = yt; break;
                case 2: y = yt; x = screenSize.y - xt; break;
                case 3: x = screenSize.y - xt; y = screenSize.x - yt; break;
                default: y = xt; x = screenSize.y - yt; break;
            }
        } else {
            switch (orientation) {
                case 1: x = yt; y = screenSize.y - xt; break;
                case 2: x = screenSize.y - xt; y = screenSize.x - yt; break;
                case 3: y = xt; x = screenSize.x - yt; break;
                default: x = xt; y = yt; break;
            }
        }
        return { x, y };
    }

    My_Vector2 GetScale()      { return touch_scale; }

    void setOrientation(int o) { orientation = o; }
    void setOtherTouch(bool p) { otherTouch  = p; }
}