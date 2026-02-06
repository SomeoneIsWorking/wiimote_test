#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <hidapi.h>
#include <SDL2/SDL.h>

const uint16_t WIIMOTE_VID = 0x057e;
const uint16_t WIIMOTE_PID = 0x0306;
const uint16_t WIIMOTE_PLUS_PID = 0x0330;

struct IRPoint {
    int x, y, size;
};

struct WiimoteDevice {
    hid_device* handle;
    std::string path;
    IRPoint ir[4];
    bool active;
};

std::mutex g_wiimote_mutex;
std::map<std::string, WiimoteDevice> g_active_wiimotes;
bool g_running = true;

bool is_device_usable(hid_device* handle) {
    if (!handle) return false;
    unsigned char report[] = { 0x15, 0x00 }; // Request status
    int res = hid_write(handle, report, sizeof(report));
    return res != -1;
}

void enable_ir(hid_device* handle) {
    auto write_reg = [&](uint32_t addr, const std::vector<uint8_t>& data) {
        unsigned char buf[22] = {0};
        buf[0] = 0x16; // Write register
        buf[1] = 0x04; // Write to EEPROM/Reg
        buf[2] = (addr >> 16) & 0xFF;
        buf[3] = (addr >> 8) & 0xFF;
        buf[4] = addr & 0xFF;
        buf[5] = (uint8_t)data.size();
        std::copy(data.begin(), data.end(), &buf[6]);
        hid_write(handle, buf, sizeof(buf));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    // Enable IR Pixel Clock
    unsigned char ir_on1[] = { 0x13, 0x04 };
    hid_write(handle, ir_on1, sizeof(ir_on1));
    unsigned char ir_on2[] = { 0x1a, 0x04 };
    hid_write(handle, ir_on2, sizeof(ir_on2));

    // Enable IR Camera
    write_reg(0xb00030, {0x08});

    // Sensitivity (Level 3 - Standard)
    write_reg(0xb00000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0x41});
    write_reg(0xb0001a, {0x40, 0x00});

    // IR Mode: Extended (3 bytes per point)
    write_reg(0xb00033, {0x03});

    // Set reporting mode to Buttons + Accel + IR (0x33)
    unsigned char mode_buf[] = { 0x12, 0x00, 0x33 };
    hid_write(handle, mode_buf, sizeof(mode_buf));
}

void scan_loop() {
    while (g_running) {
        hid_device_info *devs = hid_enumerate(WIIMOTE_VID, 0);
        std::set<std::string> found_paths;

        for (hid_device_info* cur = devs; cur != nullptr; cur = cur->next) {
            if (cur->product_id == WIIMOTE_PID || cur->product_id == WIIMOTE_PLUS_PID) {
                std::string path = cur->path;
                found_paths.insert(path);

                std::lock_guard<std::mutex> lock(g_wiimote_mutex);
                if (g_active_wiimotes.find(path) == g_active_wiimotes.end()) {
                    hid_device* handle = hid_open_path(path.c_str());
                    if (handle) {
                        if (is_device_usable(handle)) {
                            std::cout << "Connected: " << path << std::endl;
                            enable_ir(handle);
                            g_active_wiimotes[path] = { handle, path, {}, true };
                        } else {
                            hid_close(handle);
                        }
                    }
                }
            }
        }
        hid_free_enumeration(devs);

        {
            std::lock_guard<std::mutex> lock(g_wiimote_mutex);
            for (auto it = g_active_wiimotes.begin(); it != g_active_wiimotes.end(); ) {
                if (found_paths.find(it->first) == found_paths.end()) {
                    std::cout << "Disconnected: " << it->first << std::endl;
                    hid_close(it->second.handle);
                    it = g_active_wiimotes.erase(it);
                } else {
                    ++it;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

int main(int argc, char* argv[]) {
    if (hid_init()) return -1;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) return -1;

    SDL_Window* window = SDL_CreateWindow("Wiimote IR Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    std::thread scanner(scan_loop);

    while (g_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) g_running = false;
        }

        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);

        {
            std::lock_guard<std::mutex> lock(g_wiimote_mutex);
            for (auto& pair : g_active_wiimotes) {
                unsigned char buf[22];
                int res = hid_read_timeout(pair.second.handle, buf, sizeof(buf), 0);
                if (res > 0 && buf[0] == 0x33) {
                    for (int i = 0; i < 4; i++) {
                        unsigned char* ir = &buf[6 + i * 3];
                        pair.second.ir[i].x = ir[0] | ((ir[2] & 0x30) << 4);
                        pair.second.ir[i].y = ir[1] | ((ir[2] & 0xC0) << 2);
                        pair.second.ir[i].size = ir[2] & 0x0F;
                    }
                }

                // Draw IR points
                for (int i = 0; i < 4; i++) {
                    if (pair.second.ir[i].x < 1023) {
                        int x = 1023 - pair.second.ir[i].x; // Mirror for intuitive display
                        int y = pair.second.ir[i].y;
                        
                        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
                        SDL_Rect r = { x - 5, y - 5, 10, 10 };
                        SDL_RenderFillRect(renderer, &r);
                        
                        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                        SDL_RenderDrawRect(renderer, &r);
                    }
                }

                // Draw Cursor (midpoint of first two valid points)
                int valid_points = 0;
                float mid_x = 0, mid_y = 0;
                for (int i = 0; i < 2; i++) {
                    if (pair.second.ir[i].x < 1023) {
                        mid_x += (1023 - pair.second.ir[i].x);
                        mid_y += pair.second.ir[i].y;
                        valid_points++;
                    }
                }

                if (valid_points > 0) {
                    mid_x /= valid_points;
                    mid_y /= valid_points;

                    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
                    SDL_RenderDrawLine(renderer, (int)mid_x - 20, (int)mid_y, (int)mid_x + 20, (int)mid_y);
                    SDL_RenderDrawLine(renderer, (int)mid_x, (int)mid_y - 20, (int)mid_x, (int)mid_y + 20);
                }
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(10);
    }

    g_running = false;
    scanner.join();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    hid_exit();
    return 0;
}
