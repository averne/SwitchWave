#pragma once

#include <cstdint>
#include <vector>

#include <usbhsfs.h>

namespace sw::fs {

class UmsController {
    public:
        struct Device {
            UsbHsFsDeviceFileSystemType type;
            std::int32_t intf_id;
            std::string name, mount_name;
        };

        using DevicesChangedCallback = void(*)(const std::vector<Device> &, void *);

    public:
        Result initialize() {
            if (auto rc = usbHsFsInitialize(0); R_FAILED(rc))
                return rc;

            usbHsFsSetPopulateCallback(UmsController::usbhsfs_populate_cb, this);

            return 0;
        }

        void finalize() {
            usbHsFsSetPopulateCallback(nullptr, nullptr);
            this->set_devices_changed_callback(nullptr, nullptr);

            for (auto &dev: this->devices)
                this->unmount_device(dev);
            usbHsFsExit();
        }

        void set_devices_changed_callback(DevicesChangedCallback cb, void *user = nullptr) {
            this->devices_changed_cb = cb, this->devices_changed_user = user;
        }

        std::uint32_t get_num_filesystems() const {
            return usbHsFsGetMountedDeviceCount();
        }

        const std::vector<Device> &get_devices() const {
            return this->devices;
        }

        bool unmount_device(const Device &dev) {
            UsbHsFsDevice d = {
                .usb_if_id = dev.intf_id,
            };

            std::erase_if(this->devices, [&dev](const auto &d) {
                return d.mount_name == dev.mount_name;
            });

            return usbHsFsUnmountDevice(&d, true);
        }

    private:
        static void usbhsfs_populate_cb(const UsbHsFsDevice *devices, u32 device_count, void *user_data) {
            auto *self = static_cast<UmsController *>(user_data);

            self->devices.clear();
            self->devices.reserve(device_count);

            for (u32 i = 0; i < device_count; ++i) {
                auto &d = devices[i];

                std::string name;
                if (auto sv = std::string_view(d.product_name); !sv.empty())
                    name = sv;
                else if (sv = std::string_view(d.manufacturer); !sv.empty())
                    name = sv;
                else if (sv = std::string_view(d.serial_number); !sv.empty())
                    name = sv;
                else
                    name = "Unnamed device";

                self->devices.emplace_back(UsbHsFsDeviceFileSystemType(d.fs_type), d.usb_if_id, std::move(name), d.name);
            }

            if (self->devices_changed_cb)
                self->devices_changed_cb(self->devices, self->devices_changed_user);
        }

    private:
        UEvent *status_event = nullptr;
        std::vector<Device> devices;

        DevicesChangedCallback devices_changed_cb = nullptr;
        void *devices_changed_user = nullptr;
};

} // namespace sw::fs
