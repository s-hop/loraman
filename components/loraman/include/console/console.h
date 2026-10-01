#pragma once

#include "console_commands.h"
#include "utils/error.h"

#include <esp_console.h>
#include <esp_err.h>

namespace loraman::console 
{
    inline Result<void> register_commands()
    {
        auto r = register_free();
        if (!r.has_value()) return r;

        r = register_heap();
        if (!r.has_value()) return r;

        return ok();
    }

    inline Result<void> init()
    {
        esp_console_repl_t *repl = nullptr;
        esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
        const char *prompt = ">";
        repl_config.prompt = prompt;
        constexpr size_t max_cmdline_length = 256;
        repl_config.max_cmdline_length = max_cmdline_length;

        auto r = register_commands();
        if (!r.has_value()) return r;

        esp_err_t esp_err = esp_console_register_help_command();
        if (esp_err != ESP_OK) return fail(ErrCode::ConsoleRegCmdFailed, esp_err);

        esp_err = esp_console_new_repl_stdio(&repl_config, &repl);
        if (esp_err != ESP_OK) return fail(ErrCode::ConsoleInitFailed, esp_err);

        esp_err = esp_console_start_repl(repl);
        if (esp_err != ESP_OK) return fail(ErrCode::ConsoleInitFailed, esp_err);

        return ok();
    }
}