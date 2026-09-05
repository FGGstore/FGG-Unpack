/* FGG Unpack — on-console archive extraction payload.
 *
 * Reads /data/fgg-unpack/config.ini, watches the configured paths plus
 * queue.txt, and extracts anything that appears. Usable entirely over FTP.
 *
 * Send to port 9021 with a payload sender, or add fgg-unpack.elf to
 * autoload.txt for plk-autoloader.
 */
#include "ua_common.h"
#include "core/ua_config.h"
#include "core/ua_driver.h"
#include "core/ua_log.h"
#include "platform/ua_platform.h"

#include <stdio.h>

#define UA_STATE_DIR   "/data/fgg-unpack"
#define UA_CONFIG_PATH UA_STATE_DIR "/config.ini"
#define UA_LOG_PATH    UA_STATE_DIR "/debug.log"
#define UA_LOG_CAP     (2 * 1024 * 1024)

int main(void)
{
    ua_config  cfg;
    ua_driver *drv = NULL;
    ua_result  r;
    int        created = 0;
    size_t     i;

    /* The jailbreak does not survive a reboot, so the payload assumes nothing
     * about prior state beyond what it reads from disk (v2 section 3). */
    if (ua_plat_mkdirs(UA_STATE_DIR) != UA_OK) {
        ua_plat_notify("FGG Unpack: cannot create " UA_STATE_DIR);
        return 1;
    }

    ua_config_defaults(&cfg);

    /* The log opens before the config is read, so config problems are
     * themselves logged. Verbosity is corrected immediately afterwards. */
    if (ua_log_open(UA_LOG_PATH, UA_LOG_INFO, UA_LOG_CAP) != UA_OK)
        ua_plat_notify("FGG Unpack: cannot open log, continuing without it");

    UA_LOG_I("FGG Unpack starting");

    ua_config_load(UA_CONFIG_PATH, &cfg, &created);
    ua_log_open(UA_LOG_PATH, cfg.debug ? UA_LOG_DEBUG : UA_LOG_INFO, UA_LOG_CAP);

    if (created)
        UA_LOG_I("created default config at %s", UA_CONFIG_PATH);

    /* Watch paths are created rather than merely reported missing: the whole
     * workflow starts with the user uploading into one, and an FTP client
     * cannot always make the directory itself. */
    for (i = 0; i < cfg.n_watchpath; i++) {
        if (ua_plat_mkdirs(cfg.watchpath[i]) == UA_OK)
            UA_LOG_I("watching %s", cfg.watchpath[i]);
        else
            UA_LOG_W("cannot create or access watch path %s", cfg.watchpath[i]);
    }

    r = ua_driver_create(&cfg, UA_STATE_DIR, &drv);
    if (r != UA_OK) {
        UA_LOG_E("cannot start driver: %s", ua_strerror(r));

        /* Worth naming explicitly: launching twice from a payload loader is
         * easy to do by accident, and "already running" is a very different
         * thing from a real failure. */
        if (r == UA_ERR_EXISTS)
            ua_plat_notify("FGG Unpack: already running, this copy will exit");
        else
            ua_plat_notify("FGG Unpack: failed to start");
        ua_log_close();
        return 1;
    }

    if (cfg.notify) {
        char msg[160];
        snprintf(msg, sizeof msg, "FGG Unpack: watching %u path(s), polling every %us",
                 (unsigned)cfg.n_watchpath, cfg.poll_interval_seconds);
        ua_plat_notify(msg);
    }

    /* Runs until the console is rebooted or the payload is killed. The stop
     * flag exists for callers that have somewhere to set it from; this one
     * does not, so it passes NULL and runs forever. */
    ua_driver_run(drv, NULL);

    ua_driver_destroy(drv);
    ua_log_close();

    return 0;
}
