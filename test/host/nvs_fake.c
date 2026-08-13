#include "nvs.h"
#include <stdlib.h>
#include <string.h>
static char *g_blob;
esp_err_t nvs_open(const char *ns, int m, nvs_handle_t *h){ (void)ns;(void)m; *h=1; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len){
    (void)h;(void)k;
    if(!g_blob) return ESP_ERR_NVS_NOT_FOUND;
    size_t n = strlen(g_blob)+1;
    if(!out){ *len=n; return ESP_OK; }
    if(*len < n) return ESP_ERR_INVALID_SIZE;
    memcpy(out,g_blob,n); *len=n; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v){ (void)h;(void)k; free(g_blob); g_blob=strdup(v); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h){ (void)h; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k){ (void)h;(void)k; free(g_blob); g_blob=NULL; return ESP_OK; }
void nvs_close(nvs_handle_t h){ (void)h; }
