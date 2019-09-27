#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "bemanitools/eamio.h"
#include "bemanitools/iidxio.h"

#include "cconfig/cconfig-hook.h"

#include "hooklib/acp.h"
#include "hooklib/adapter.h"
#include "hooklib/app.h"
#include "hooklib/rs232.h"
#include "hooklib/setupapi.h"

#include "iidxhook-util/acio.h"
#include "iidxhook-util/config-gfx.h"
#include "iidxhook-util/d3d9.h"
#include "iidxhook-util/log-server.h"

#include "iidxhook8/bio2.h"
#include "iidxhook8/cam.h"
#include "iidxhook8/config-cam.h"
#include "iidxhook8/config-io.h"
#include "iidxhook8/setupapi.h"
#include "iidxhook8/cam.h"

#include "imports/avs.h"

#include "util/log.h"
#include "util/str.h"
#include "util/thread.h"

#define IIDXHOOK8_INFO_HEADER \
    "iidxhook for Cannon Ballers" \
    ", build " __DATE__ " " __TIME__ ", gitrev " STRINGIFY(GITREV) "\n"
#define IIDXHOOK8_CMD_USAGE \
    "Usage: launcher.exe -K iidxhook8.dll <bm2dx.dll> [options...]"

static const irp_handler_t iidxhook_handlers[] = {
    iidxhook_util_acio_dispatch_irp,
    bio2_port_dispatch_irp,
};

struct iidxhook8_config_io iidxhook8_config_io;

static bool my_dll_entry_init(char *sidcode, struct property_node *param)
{
    struct cconfig* config;
    
    struct iidxhook_config_gfx config_gfx;
    struct iidxhook8_config_cam config_cam;

    // log_server_init is not required anymore

    log_info("-------------------------------------------------------------");
    log_info("--------------- Begin iidxhook dll_entry_init ---------------");
    log_info("-------------------------------------------------------------");

    config = cconfig_init();

    iidxhook_config_gfx_init(config);
    iidxhook8_config_cam_init(config);
    iidxhook8_config_io_init(config);

    if (!cconfig_hook_config_init(config, IIDXHOOK8_INFO_HEADER "\n" IIDXHOOK8_CMD_USAGE, CCONFIG_CMD_USAGE_OUT_DBG)) {
        cconfig_finit(config);
        log_server_fini();
        exit(EXIT_FAILURE);
    }

    iidxhook_config_gfx_get(&config_gfx, config);
    iidxhook8_config_cam_get(&config_cam, config);
    iidxhook8_config_io_get(&iidxhook8_config_io, config);

    cconfig_finit(config);

    log_info(IIDXHOOK8_INFO_HEADER);
    log_info("Initializing iidxhook...");

    if (config_gfx.windowed) {
        d3d9_set_windowed(config_gfx.framed, config_gfx.window_width,
            config_gfx.window_height);
    }
    
    if (config_gfx.pci_id_pid != 0 && config_gfx.pci_id_vid != 0) {
        d3d9_set_pci_id(config_gfx.pci_id_pid, config_gfx.pci_id_vid);
    }

    if (config_gfx.frame_rate_limit > 0) {
        d3d9_set_frame_rate_limit(config_gfx.frame_rate_limit);
    }

    if (config_gfx.scale_back_buffer_width > 0 && config_gfx.scale_back_buffer_height > 0) {
        d3d9_scale_back_buffer(config_gfx.scale_back_buffer_width, config_gfx.scale_back_buffer_height,
            config_gfx.scale_back_buffer_filter);
    }

    /* Start up IIDXIO.DLL */
    if (!iidxhook8_config_io.disable_bio2_emu) {
        log_info("Starting IIDX IO backend");
        iidx_io_set_loggers(log_impl_misc, log_impl_info, log_impl_warning,
                log_impl_fatal);

        if (!iidx_io_init(avs_thread_create, avs_thread_join, avs_thread_destroy)) {
            log_fatal("Initializing IIDX IO backend failed");
        }
    }

    /* Start up EAMIO.DLL */
    if (!iidxhook8_config_io.disable_card_reader_emu) {
        log_misc("Initializing card reader backend");
        eam_io_set_loggers(log_impl_misc, log_impl_info, log_impl_warning,
                log_impl_fatal);

        if (!eam_io_init(avs_thread_create, avs_thread_join, avs_thread_destroy)) {
            log_fatal("Initializing card reader backend failed");
        }
    }

    /* iohooks are okay, even if emu is diabled since the fake handlers won't be used */
    /* Set up IO emulation hooks _after_ IO API setup to allow
       API implementations with real IO devices */
    iohook_init(iidxhook_handlers, lengthof(iidxhook_handlers));
    rs232_hook_init();

    if (!iidxhook8_config_io.disable_bio2_emu) {
        bio2_port_init(iidxhook8_config_io.disable_poll_limiter);
        setupapi_hook_init();
    }

    if (!iidxhook8_config_io.disable_card_reader_emu) {
        iidxhook_util_acio_init(false);
    }

    // camera hooks
    if (!config_cam.disable_emu) {
        cam_hook_init(config_cam.device_id1, config_cam.device_id2);
    }

    log_info("-------------------------------------------------------------");
    log_info("---------------- End iidxhook dll_entry_init ----------------");
    log_info("-------------------------------------------------------------");

    return app_hook_invoke_init(sidcode, param);
}

static bool my_dll_entry_main(void)
{
    bool result;

    result = app_hook_invoke_main();

    if (!iidxhook8_config_io.disable_card_reader_emu) {
        log_misc("Shutting down card reader backend");
        eam_io_fini();
    }

    if (!iidxhook8_config_io.disable_bio2_emu) {
        log_misc("Shutting down IIDX IO backend");
        iidx_io_fini();
    }

    return result;
}

/**
 * Hook library Resort Anthem onwards
 */
BOOL WINAPI DllMain(HMODULE mod, DWORD reason, void *ctx)
{
    if (reason != DLL_PROCESS_ATTACH) {
        goto end;
    }

    log_to_external(
            log_body_misc,
            log_body_info,
            log_body_warning,
            log_body_fatal);

    app_hook_init(my_dll_entry_init, my_dll_entry_main);

    acp_hook_init();
    adapter_hook_init();
    d3d9_hook_init();

end:
    return TRUE;
}
