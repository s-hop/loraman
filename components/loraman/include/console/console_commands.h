#pragma once

#include "utils/error.h"

#include <esp_console.h>
#include <esp_err.h>
#include <esp_heap_caps.h>

#include <cinttypes>
#include <cstdio>

namespace loraman::console
{
    static int free_mem(int argc, char **argv)
    {
        printf("%" PRIu32 "\n", esp_get_free_heap_size());
        return 0;
    }

    static Result<void> register_free()
    {
        esp_console_cmd_t cmd{};
        cmd.command = "free";
        cmd.help = "Get the current size of free heap memory";
        cmd.func = &free_mem;

        esp_err_t result = esp_console_cmd_register(&cmd);
        if (result != ESP_OK) {
            return fail(ErrCode::ConsoleRegCmdFailed, result);
        }

        return ok();
    }

    static int heap_size(int argc, char **argv)
    {
        uint32_t heap_size = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        printf("min heap size: %" PRIu32 "\n", heap_size);
        return 0;
    }

    static Result<void> register_heap(void)
    {
        esp_console_cmd_t heap_cmd{};
        heap_cmd.command = "heap";
        heap_cmd.help = "Get minimum size of free heap memory that was available during program execution";
        heap_cmd.func = &heap_size;

        esp_err_t result = esp_console_cmd_register(&heap_cmd);
        if (result != ESP_OK) {
            return fail(ErrCode::ConsoleRegCmdFailed, result);
        }

        return ok();
    }
}